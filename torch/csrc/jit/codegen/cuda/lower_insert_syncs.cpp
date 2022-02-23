#include <torch/csrc/jit/codegen/cuda/dispatch.h>
#include <torch/csrc/jit/codegen/cuda/instrumentation.h>
#include <torch/csrc/jit/codegen/cuda/ir_utils.h>
#include <torch/csrc/jit/codegen/cuda/kernel_ir.h>
#include <torch/csrc/jit/codegen/cuda/kernel_ir_dispatch.h>
#include <torch/csrc/jit/codegen/cuda/lower2device.h>
#include <torch/csrc/jit/codegen/cuda/lower_insert_syncs.h>

#include <unordered_set>

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

namespace {

//! Scan through Kernel IR for-loops to insert Sync nodes to avoid
//! Write-After-Read (WAR) race condition.
//!
//! Example:
//!   for () {
//!     smem_buf[threadIdx.x] = x;
//!     __syncthreads();
//!     buf[threadId.x] = smem_buf[threadIdx.x + 1];
//!  }
//!
//! In this case, additional syncthreads is needed at the end of the
//! loop body to avoid a hazard with smem_buf.

//! Keeping track the allocations of SMEM TVs
class SmemAllocMap {
 public:
  //! Insert a new node if it's a SMEM allocation
  void insert(kir::Allocate* alloc) {
    if (auto tv = dynamic_cast<TensorView*>(alloc->buffer())) {
      if (tv->getMemoryType() == MemoryType::Shared) {
        // Note that a TensorView can have two allocations due to
        // unswitch.
        auto p = map_.insert({tv, alloc});
        // If there's an existing entry, reset it with the new
        // alloc. Currently, the existing alloc is actually the same
        // as the new one as each expression is just inserted to both
        // then and else parts of the unswitched loop, but this should
        // be changed.
        if (!p.second) {
          p.first->second = alloc;
        }
      }
    }
  }

  //! Run through aliases to get the buffer that is actually allocated for a
  //! given TV
  TensorView* getRealBuffer(TensorView* tv) const {
    auto it = map_.find(tv);
    TORCH_INTERNAL_ASSERT(
        it != map_.end(), "Allocation not found for ", tv->toString());
    const kir::Allocate* alloc = it->second;
    while (alloc->alias()) {
      alloc = alloc->alias();
    }
    auto buf = alloc->buffer();
    TORCH_INTERNAL_ASSERT(buf->isA<TensorView>());
    return buf->as<TensorView>();
  }

 private:
  std::unordered_map<TensorView*, kir::Allocate*> map_;
};

struct WarMemoryInfo {
  // True if there's a sync after the last read within the alloc loop.
  bool sync_after_read = false;

  // True if there's a sync before the first write. There can be multiple writes
  // from memory aliasing.
  bool sync_before_write = false;

  // Has there been a read of this memory location
  bool read_hit = false;

  // Has there been *the* write to this memory location, assumes single write
  // instruction (needs to be before conditionals added to code)
  bool write_hit = false;

  // For loop this TV is compute_at'ed in.
  kir::ForLoop* ca_loop = nullptr;
};

// To prevent shared memory from being over written before it is read, a
// synchronization point has to be inserted either between the allocation of an
// SMEM buffer and where we write into it, or after the buffer's last read
// before exiting the allocation's scope.
//
// e.g.
//  for i:
//    "alloc A" in shared memory - This is really marked by the compute_at point
//    sync_loc_0
//    for j:
//      sync_loc_1
//      for k:
//        sync_loc_2
//        A = ...
//      for k:
//        ... = ... A
//    for j:
//      for k:
//        ... = ... A
//        sync_loc_3
//      sync_loc_4
//    sync_loc_5
//
// All sync locations here provide valid protection that memory in A is finished
// being read before it is over written in the next iteration
//
// Insertion of sync threads will be done from the inner most position to the
// outer most. If a sync protecting the buffer is not already placed, the
// location prefered for the sync threads is the last possible position. One
// future optimization could be to not sync on the last iteration of the loop
// the sync is placed in.
class WarSyncInserter : private kir::ExprMutator {
 public:
  static std::vector<Expr*> insert(const std::vector<Expr*>& exprs) {
    WarSyncInserter inserter(exprs);
    return inserter.exprs_;
  }

 private:
  //! Insert Sync nodes at the end of a given for-loop when a WAR
  //! hazard may happen.
  WarSyncInserter(const std::vector<Expr*>& exprs) {
    auto& lower_alloc_info_map = GpuLower::current()->localAllocationInfoMap();
    for (const auto& entry : lower_alloc_info_map) {
      alloc_map_.insert(entry.first);
    }
    kir::ExprMutator::traverseAndInsert(exprs);
  }

  void handle(kir::IfThenElse* ite) final {
    TORCH_INTERNAL_ASSERT(
        ite->elseBody().empty(),
        "Pass does not support conditional flow,",
        " needs to be done before conditional execution is lowered.");
    kir::ExprMutator::handle(ite);
  }

  void handle(kir::Sync* sync) final {
    // Register the sync for the active for loop
    sync_hit_.back() = true;
    // Run through the active allocations, if a read was hit, register there was
    // a sync after the read. If there's subsequent reads on this buffer the
    // sync_after_read will be cleared.
    for (auto& entry : smem_allocations_) {
      auto& alloc_stack = entry.second;
      if (alloc_stack.back().read_hit) {
        alloc_stack.back().sync_after_read = true;
      }
    }
  }

  // Checks if fl or loops within it have hit a sync
  bool syncWithin(kir::ForLoop* fl) {
    // If outer most scope check the first sync_hit_ position
    if (fl == nullptr) {
      return sync_hit_[0];
    }

    // Find the for loop we want to look within
    auto fl_it = std::find(for_loops_.begin(), for_loops_.end(), fl);

    // Convert it to an index, but add one for the outer most scope
    auto fl_i = std::distance(for_loops_.begin(), fl_it) + 1;

    // Start at that index and see if there's syncs within that for loop
    for (auto i : c10::irange(fl_i, sync_hit_.size())) {
      if (sync_hit_[i]) {
        return true;
      }
    }
    return false;
  }

  void handle(Expr* expr) final {
    // If not a tensor view expression continue with dispatch
    if (!ir_utils::isTvOp(expr)) {
      kir::ExprMutator::handle(expr);
      return;
    }

    // Mark write has been hit for all output tvs
    auto out_tvs = ir_utils::filterByType<TensorView>(expr->outputs());
    for (auto out_tv : out_tvs) {
      if (out_tv->getMemoryType() != MemoryType::Shared) {
        continue;
      }
      auto& entry = getMemInfo(out_tv);

      // If this is the first write and there's a sync in one of the loops after
      // the compute at loop, then this buffer is protected.
      if (syncWithin(entry.ca_loop) && !entry.write_hit) {
        entry.sync_before_write = true;
      }
      entry.write_hit = true;
    }

    // Mark read was hit, if sync_after_read was set, clear it.
    auto inp_tvs = ir_utils::filterByType<TensorView>(expr->inputs());
    for (auto inp_tv : inp_tvs) {
      if (inp_tv->getMemoryType() != MemoryType::Shared) {
        continue;
      }
      auto& entry = getMemInfo(inp_tv);
      entry.read_hit = true;
      // Clear the sync_after_read if it was set because there was another write
      entry.sync_after_read = false;
    }
  }

  void handle(kir::ForLoop* for_loop) final {
    // Push loop scope information
    auto prev_within_iter_loop_ = within_iter_loop_;
    sync_hit_.push_back(false);

    // If there is no real iterating loop WAR syncs aren't necessary
    within_iter_loop_ = within_iter_loop_ ||
        !(for_loop->iter_domain()->isThread() ||
          for_loop->iter_domain()->isBroadcast() ||
          for_loop->iter_domain()->extent()->isOneInt());

    // Process the expressions in the for loop
    kir::ExprMutator::handle(for_loop);

    // Sync analysis and cleanup:
    //
    //   Pop for loop stack inside WarMemoryInfo structs if they match this one.
    //   Erase empty entries so we don't continue to search over them
    //
    //   Insert sync at end of this for loop if any of the entries require
    std::vector<TensorView*> to_erase;
    bool insert_sync = false;
    for (auto& entry : smem_allocations_) {
      auto& alloc_stack = entry.second;
      if (alloc_stack.size() && alloc_stack.back().ca_loop == for_loop) {
        if (!alloc_stack.back().sync_after_read &&
            !alloc_stack.back().sync_before_write) {
          insert_sync = within_iter_loop_;
        }

        alloc_stack.pop_back();
        if (alloc_stack.empty()) {
          to_erase.push_back(entry.first);
        }
      }
    }

    for (auto tv : to_erase) {
      smem_allocations_.erase(tv);
    }

    // WAR Sync is necessary in this loop, register its insertion.
    if (insert_sync) {
      auto sync_expr = IrBuilder::create<kir::Sync>(true);
      kir::ExprMutator::registerInsertAfter(
          for_loop->body().exprs().back(), sync_expr, &for_loop->body());
      handle(sync_expr);
    }

    // Pop for loop scope information
    sync_hit_.pop_back();
    within_iter_loop_ = prev_within_iter_loop_;
  }

  // Create a new WarMemoryInfo entry if required and return a reference to it,
  // else return the WarMemoryInfo associated with tv
  WarMemoryInfo& getMemInfo(TensorView* tv) {
    auto maybe_aliased_tv = alloc_map_.getRealBuffer(tv);
    auto alloc_it = smem_allocations_.find(maybe_aliased_tv);
    auto ca_loop =
        loop_utils::getAllocInformation(tv, for_loops_).init_for_loop;
    if (alloc_it == smem_allocations_.end()) {
      WarMemoryInfo mem_info;
      mem_info.ca_loop = ca_loop;
      auto entry_it =
          smem_allocations_
              .insert(std::make_pair(
                  maybe_aliased_tv, std::vector<WarMemoryInfo>({mem_info})))
              .first;
      return entry_it->second.back();
    } else if (
        maybe_aliased_tv != tv && alloc_it->second.back().ca_loop != ca_loop) {
      WarMemoryInfo mem_info;
      mem_info.ca_loop = ca_loop;
      auto& alloc_stack = alloc_it->second;
      alloc_stack.push_back(mem_info);
      return alloc_stack.back();
    }
    return alloc_it->second.back();
  }

  //! Allocation map of SMEM buffers. Needed because of SMEM buffer aliasing,
  //! need to track the root of the alias to properly insert WAR hazard syncs
  SmemAllocMap alloc_map_;

  //! Is there a loop nest that has a non-trivial iteration (extent != 1) and
  //! not bound to a block/thread. This indicates if a WAR sync is necessary,
  //! otherwise the Expr is not in an iterating for loop.
  bool within_iter_loop_ = false;

  // Track which loops have hit a sync. Used to see if there's a sync before
  // write.
  std::vector<bool> sync_hit_ = {false};

  // Keep track of the active allocations we need to protect. Key is the
  // "getRealBuffer", not the raw tv. There can be multiple WarMemoryInfo's
  // because of aliasing. If the "getRealBuffer" tv has a compute at outside the
  // alias tv, each aliased tv in a unique ca_loop has to be tracked separately
  // for WAR insertion.
  std::unordered_map<TensorView*, std::vector<WarMemoryInfo>> smem_allocations_;
};

class ExprFlattener : private kir::IrVisitor {
 private:
  using kir::IrVisitor::handle;

  void handle(Expr* expr) final {
    if (expr->isA<kir::ForLoop>() || expr->isA<kir::IfThenElse>()) {
      kir::IrVisitor::handle(expr);
    } else {
      flat_exprs_.push_back(expr);
    }
  }

 private:
  std::vector<Expr*> flat_exprs_;

 public:
  //! Flattens scopes extracting out a single ordered list of exprs.
  static std::vector<Expr*> flatten(const std::vector<Expr*>& loop_nests) {
    ExprFlattener flattener;
    for (auto expr : loop_nests) {
      flattener.handle(expr);
    }
    return flattener.flat_exprs_;
  }
};

class ValidatePlacementAfterWrites : private kir::IrVisitor {
 public:
  //! Validate no expr in writes found under loop
  static void validate(
      kir::ForLoop* loop,
      const std::unordered_set<Expr*>& writes) {
    ValidatePlacementAfterWrites validator(writes);
    validator.handle(loop);
  }

 private:
  using kir::IrVisitor::handle;

  ValidatePlacementAfterWrites(const std::unordered_set<Expr*>& writes)
      : writes_(writes) {}

  void handle(Expr* expr) final {
    if (expr->isA<kir::ForLoop>() || expr->isA<kir::IfThenElse>()) {
      kir::IrVisitor::handle(expr);
    } else {
      TORCH_INTERNAL_ASSERT(
          writes_.find(expr) == writes_.end(),
          "Block sync must be placed after ",
          expr->toString());
    }
  }

 private:
  const std::unordered_set<Expr*>& writes_;
};

class ReadAfterWriteSyncs : public kir::ExprMutator {
 private:
  using kir::ExprMutator::handle;

  //! Traverse up the loop stack from loops_it and if a halo loop is
  //! found, place a given sync expr before the outer-most halo loop.
  bool insertBeforeHaloLoop(
      std::vector<kir::ForLoop*>::iterator loops_it,
      kir::Sync* sync_expr,
      const std::unordered_set<Expr*>& writes) {
    std::vector<kir::ForLoop*>::iterator halo_loop_it;
    bool halo_loop_found = false;

    while (true) {
      if ((*loops_it)->iter_domain()->isThreadDim() &&
          (*loops_it)->iter_domain()->extent() != (*loops_it)->stop()) {
        halo_loop_found = true;
        halo_loop_it = loops_it;
      }

      if (loops_it == for_loops_.begin()) {
        break;
      }
      --loops_it;
    }

    // No halo loop found. Do not place the sync expr here. Return
    // false to indicate nothing is done.
    if (!halo_loop_found) {
      return false;
    }

    auto halo_loop = *halo_loop_it;

    // Make sure there's no write to the smem buffer inside the halo
    // loop. syncthreads is moved before the halo loop, so having
    // writes inside the loop invalidates the consistency.
    ValidatePlacementAfterWrites::validate(halo_loop, writes);

    if (halo_loop_it == for_loops_.begin()) {
      // place in global scope
      auto place_before_it = std::find(exprs_.begin(), exprs_.end(), halo_loop);
      TORCH_INTERNAL_ASSERT(place_before_it != exprs_.end());
      exprs_.insert(place_before_it, sync_expr);
    } else {
      auto place_in = *(halo_loop_it - 1);
      kir::ExprMutator::registerInsertBefore(
          halo_loop, sync_expr, &place_in->body());
    }

    return true;
  }

  void handle(Expr* expr) final {
    if (!ir_utils::isTvOp(expr) || expr->isA<kir::Allocate>()) {
      kir::ExprMutator::handle(expr);
      return;
    }

    if (sync_after_.size() > 0 && sync_after_.front() == expr) {
      sync_after_.pop_front();
      auto last_writes = last_writes_.front();
      last_writes_.pop_front();
      // Found that a sync is needed
      TORCH_INTERNAL_ASSERT(expr->outputs()[0]->isA<TensorView>());
      auto out_tv = expr->outputs()[0]->as<TensorView>();

      // Find where a sync needs to be inserted
      // This is very similar to how allocations are placed, simply place sync
      // after the expression instead of placing like allocation where it goes
      // before.
      // TODO: This may be a common operation, could be worth making a utility
      // out of or saving state for tensor view ID -> for loop
      // TODO: Explicitly test the 3 cases below

      auto sync_expr = IrBuilder::create<kir::Sync>();
      if (out_tv->getComputeAtPosition() == 0) {
        // Sync should be placed at global scope, after its outer most loop if
        // it has one.
        Expr* place_after = for_loops_.size() > 0 ? for_loops_[0] : expr;
        // Find location in exprs_
        auto place_after_it =
            std::find(exprs_.begin(), exprs_.end(), place_after);
        TORCH_INTERNAL_ASSERT(
            place_after_it != exprs_.end(),
            "Could not figure out where to place synchronization. ",
            "Tried to place after, ",
            place_after->toString(),
            ", but could not find this expression at the global scope.");

        registerInsertAfter(*(place_after_it + 1), sync_expr, nullptr);
      } else {
        // Find the last loop in computeAt of out_tv, this is the loop where we
        // would place an allocation for out_tv
        auto local_id = out_tv->axis((int)out_tv->getComputeAtPosition() - 1);

        auto loops_it = std::find_if(
            for_loops_.begin(),
            for_loops_.end(),
            [&local_id](const auto& loop) {
              return GpuLower::current()->caLoopMap().areMapped(
                         loop->iter_domain(), local_id) ||
                  loop->iter_domain()->getParallelType() ==
                  ParallelType::Unroll;
            });

        TORCH_INTERNAL_ASSERT(loops_it != for_loops_.end());

        // block sync must be placed before halo-extended loops
        if (insertBeforeHaloLoop(loops_it, sync_expr, last_writes)) {
          return;
        }

        auto place_in = *loops_it;
        Expr* place_after = nullptr;

        if (loops_it + 1 == for_loops_.end()) {
          // Inline allocation, place after expr
          place_after = expr;
        } else {
          // Place allocation after the last computeAt axis
          // TODO: may be more efficient to place after the first non-computeAt
          // axis
          place_after = *(loops_it + 1);
        }

        registerInsertAfter(place_after, sync_expr, &place_in->body());
      }
    }
  }

  void handle(kir::IfThenElse*) final {
    TORCH_INTERNAL_ASSERT(
        false,
        "Pass does not support conditional statements, ",
        "this pass should be run before any conditionals are placed in code.");
  }

  // Clear the modify status for all shared memory buffers
  static void cleanSharedMemory(std::unordered_map<Val*, Expr*>& smem) {
    smem.clear();
  }

  // Return a set of expressions that modify shared-memory
  // tensors. Expressions are excluded when syncthreads are already
  // placed.
  std::unordered_set<Expr*> isModifiedSharedMemory(
      const std::unordered_map<Val*, Expr*>& smem,
      const std::vector<Val*>& tvs) const {
    std::unordered_set<Expr*> last_writes;
    for (auto tv : tvs) {
      auto it = smem.find(tv);
      if (it != smem.end()) {
        last_writes.insert(it->second);
      }
    }
    return last_writes;
  }

  ReadAfterWriteSyncs(const std::vector<Expr*>& _exprs) {
    // Fusion shared_memory values
    // Tracks if shared memory is modified
    std::unordered_map<Val*, Expr*> smem;

    // Flatten all the expressions
    auto flattened_exprs = ExprFlattener::flatten(_exprs);

    Expr* prev_tv_expr = nullptr;
    for (auto expr : flattened_exprs) {
      if (!ir_utils::isTvOp(expr) || expr->isA<kir::Allocate>()) {
        continue;
      }

      auto last_writes = isModifiedSharedMemory(smem, expr->inputs());
      if (!last_writes.empty()) {
        TORCH_INTERNAL_ASSERT(
            prev_tv_expr != nullptr,
            "Can't require sync on inputs, however, detected it's needed.");
        sync_after_.push_back(prev_tv_expr);
        last_writes_.push_back(last_writes);
        cleanSharedMemory(smem);
      }

      for (auto tv : ir_utils::filterByType<TensorView>(expr->outputs())) {
        // Double buffered tensors do not need RAW sync to be inserted
        // here, except for the initial load part, which is taken care
        // separately by DoubleBufferInserter.
        if (tv->getMemoryType() == MemoryType::Shared &&
            !tv->isDoubleBuffered()) {
          smem[tv] = expr;
        }
      }

      prev_tv_expr = expr;
    }

    kir::ExprMutator::traverseAndInsert(_exprs);

    TORCH_INTERNAL_ASSERT(
        sync_after_.empty(), "Didn't place all required syncs.");
  }

 private:
  //! Keep track of expressions that must be followed by syncthreads
  std::deque<Expr*> sync_after_;

  //! Keep track of write expressions that must be placed before
  //! syncthreads.
  //!
  //! syncthreads is placed after for each expression of
  //! sync_after_. However, if it's inside a loop with halo, it must
  //! be placed before that. last_writes_ keeps track of expressions
  //! modifying the smem buffer each syncthreads is used for so that
  //! it is not placed before those write expressions.
  std::deque<std::unordered_set<Expr*>> last_writes_;

 public:
  static std::vector<Expr*> insert(const std::vector<Expr*>& loop_nests) {
    ReadAfterWriteSyncs inserter(loop_nests);
    return inserter.exprs_;
  }
};

} // namespace

std::vector<Expr*> insertRawThreadSynchronization(
    const std::vector<Expr*>& exprs) {
  FUSER_PERF_SCOPE("GpuLower::Lower::insertRawThreadSynchronization");
  return ReadAfterWriteSyncs::insert(exprs);
}

std::vector<Expr*> insertWarThreadSynchronization(
    const std::vector<Expr*>& exprs) {
  FUSER_PERF_SCOPE("GpuLower::Lower::insertWarThreadSynchronization");
  return WarSyncInserter::insert(exprs);
}
} // namespace cuda
} // namespace fuser
} // namespace jit
} // namespace torch
