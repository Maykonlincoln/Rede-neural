#include <torch/csrc/jit/fuser/common/fusion.h>
#include <torch/csrc/jit/fuser/common/ir.h>
#include <torch/csrc/jit/fuser/common/iriostream.h>
#include <torch/csrc/jit/fuser/common/tensor.h>
#include <torch/csrc/jit/fuser/common/transform_replay.h>
#include <torch/csrc/jit/fuser/common/type.h>

#include <torch/csrc/jit/fuser/common/code_write.h>

namespace torch {
namespace jit {
namespace fuser {

std::vector<Int*> CodeWrite::getLoopIndices() {
  std::vector<Int*> inds;
  for (auto pair : fors)
    inds.push_back(pair.first);
  return inds;
}

void CodeWrite::print_indices(const std::vector<Int*>& indices) {
  os << "[";
  for (const auto& ind : indices) {
    Printer(os).print_inline(ind);
    if (ind != *(indices.end() - 1))
      os << ", ";
  }
  os << "]";
}

void CodeWrite::print(const TensorView* const tv) {
  TensorDomain* td = tv->domain();

  const TensorView* tv2 = tv;

  if (producer && consumer != nullptr) {
    // get new reference so replay inline doesn't change the original.
    TensorView* tv_ = tv->clone();
    tv_->resetView();
    TransformReplay::fullReplay(consumer, tv_);
    tv2 = tv_;
  } else if (producer) {
    throw std::runtime_error(
        "Could not find consumer for producer in CodeWrite.");
  }

  if (tv2->tensor() != nullptr) {
    os << "T" << tv2->tensor()->name();
  } else {
    os << "TV" << tv2->name();
  }

  std::vector<Int*> indices =
      IndexCompute::computeIndices(tv2, getLoopIndices());

  print_indices(indices);
}

void CodeWrite::print(const Val* const val) {
  if (*(val->getValType()) == ValType::TensorView)
    print(static_cast<const TensorView* const>(val));
  else 
   Printer::print(val);
}

bool CodeWrite::print_predicate(const TensorView* const pred_tv) {  
  std::vector<Int*> indices =
    IndexCompute::computeIndices(pred_tv, getLoopIndices());

  std::vector<Int*> preds = PredicateCompute::computePredicates (pred_tv, indices);

  if(preds.size() == 0)
    return false;
  
  bool first_pred = true;
  os << "if( ";
  for(decltype(preds.size()) i{0}; i < preds.size(); i++){
    if(preds[i]->same_as(new Int(1.0)))
      continue;
    if(!first_pred)
      os << " && ";

    print_inline(preds[i]);

    first_pred = false;
    
  }
  os  << " ) {\n";
  ++extra_indent;
  indent();
  return true;
  


}

bool CodeWrite::print_lhs(TensorView* tv){
  updateView(tv);
  indent();

  //Print predicates, first need to find predicate.
  bool predicated = print_predicate(tv);

  print(tv);
  os << " = ";

  consumer = tv;
  producer = true;

  return predicated;
}

//Already filtered so output is a TensorView
void CodeWrite::print(const UnaryOp* const uop) {
  if(!isTVOp(uop)){
    if(print_inline_)
      Printer::print(uop);
    return;
  }

  bool predicated = print_lhs(static_cast<TensorView*>(uop->out()));

  if (auto inline_uop = inline_op_str(uop->type())) {
    os << inline_uop.value();
    print(uop->in());
  } else {
    os << uop->type() << "(";
    print(uop->in());
    os << ")";
  }

  consumer = nullptr;
  producer = false;

  os << ";\n";

  if(predicated){
    --extra_indent;
    indent();
    os << "}\n";
  }

}

void CodeWrite::print(const BinaryOp* const bop) {

  if(!isTVOp(bop)){
    if(print_inline_)
      Printer::print(bop);
    return;
  }

  bool predicated = print_lhs(static_cast<TensorView*>(bop->out()));

  if (auto inline_bop = inline_op_str(bop->type())) {
    print(bop->lhs());
    os << " " << inline_bop.value() << " ";
    print(bop->rhs());
  } else {
    os << bop->type() << "(";
    print(bop->lhs());
    os << ", ";
    print(bop->rhs());
    os << ")";
  }

  consumer = nullptr;
  producer = false;

  os << ";\n";

  if(predicated){
    --extra_indent;
    indent();
    os << "}\n";
  }

}

void CodeWrite::indent() {
  for (int i = 0; i < fors.size() + extra_indent; i++)
    os << "  ";
}

void CodeWrite::closeScope() {
  fors.pop_back();
  indent();
  os << "}" << std::endl;
}

void CodeWrite::openFor(IterDomain* id) {
  indent();
  fors.push_back(std::pair<Int*, Int*>{new Int(), id->size()});

  os << "for( " << fors.back().first << " : ";
  print_inline(id);
  os << " ) {" << std::endl;
}

void CodeWrite::clearActiveView() {
  active_view_axis = 0;
  active_view = nullptr;
}

void CodeWrite::resetFors() {
  while (!fors.empty())
    closeScope();

  reset_fors = false;
  clearActiveView();
}


// Update fors based on tv.
void CodeWrite::updateView(TensorView* tv) {
  // If previous tv flaged that fors need to be reset, clear them all
  if (reset_fors)
    resetFors();

  // Hit the final statment in a string of compute_at's. Need to close the for
  // loops down to the previous compute_at axis, then need to put my remaining
  // for loops on there. Also need to set reset_fors flag.

  // tv is not part of a computeAt structure, or it's the final tv in a 
  // computeAt structure.
  if (!tv->hasComputeAt()) {
    // If we're the last computeAt of a block of computeAt TVs.
    if (active_view != nullptr && tv->same_as(active_view)) {
      int depth = fors.size();
      // reduce down to previous active view_axis
      for (int i = active_view_axis; i < depth; i++)
        closeScope();
      //Remove the active view
      clearActiveView();
    } else {
    // I'm not the final computeAt of a block, I'm independent.
    // Reset the loop structure
      resetFors();
    }
    for (int i = fors.size(); i < tv->domain()->size(); i++)
      openFor(tv->domain()->axis(i));
    reset_fors = true;
  } else {
    active_view_axis = tv->getComputeAtAxis();
    active_view = tv->getComputeAtView();

    int depth = fors.size();
    for (int i = active_view_axis; i < depth; i++)
      closeScope();
    for (int i = fors.size(); i < tv->domain()->size(); i++)
      openFor(tv->domain()->axis(i));
  }
}

bool CodeWrite::isTVOp(const Expr* expr){
  if(expr->nOutputs() == 1 && expr->output(0)->getValType().value() == ValType::TensorView)
    if(expr->getExprType().value() == ExprType::BinaryOp
    || expr->getExprType().value() == ExprType::UnaryOp)
      return true;
  return false;
}

void CodeWrite::traverse(
    const Fusion* const fusion,
    bool from_outputs_only,
    bool breadth_first,
    std::unordered_set<ValType> val_types) {
  //IterVisitor::traverse(fusion, from_outputs_only, breadth_first, val_types);
  std::vector<Expr*> exprs = FusionGuard::getCurFusion()->exprs();
  for(auto* expr : exprs)
    Printer::print(expr);
  resetFors();
}

} // namespace fuser
} // namespace jit
} // namespace torch