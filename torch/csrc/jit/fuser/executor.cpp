#include "torch/csrc/jit/fuser/executor.h"

#include "ATen/ATen.h"
#include "ATen/ExpandUtils.h"
#include "c10/util/Optional.h"
#include "torch/csrc/utils/functional.h"
#include "torch/csrc/jit/stack.h"
#include "torch/csrc/jit/fuser/interface.h"
#include "torch/csrc/jit/fuser/kernel_cache.h"
#include "torch/csrc/jit/fuser/kernel_spec.h"
#include "torch/csrc/jit/fuser/compiler.h"
#include "torch/csrc/jit/fuser/common/tensor_info.h"

#include <vector>
#include <tuple>
#include <stdexcept>
#include <algorithm>
#include <map>
#include <iostream> // TODO: remove, debugging only

namespace torch { namespace jit { namespace fuser {

static c10::optional<std::vector<int64_t>> getMapSize(
  const KernelSpec& spec
, at::TensorList args
, at::IntList arg_subset) {
  
  int64_t dim_after_broadcast = 0;
  for (const auto arg_idx : arg_subset) {
    dim_after_broadcast = std::max(dim_after_broadcast, args[arg_idx].dim());
  }
  // TODO: this keeps reallocating map_size at every iteration, but we know
  // exactly how much storage do we need, so this could be fixed in-place at
  // every step. We're just missing a few functions for ATen, but the fix
  // should be straightforward.
  // Note: left unitialized since empty shape is broadcastable to any shape
  std::vector<int64_t> map_size;
  for (size_t i = 0; i < arg_subset.size(); ++i) {
    auto& arg = args.at(arg_subset[i]);
    auto& chunk_desc = spec.inputChunks().at(arg_subset[i]);
    if (chunk_desc.nSubTensors() == 1) {
      try {
        map_size = at::infer_size(map_size, arg.sizes());
      } catch (...) {
        return c10::nullopt;
      }
    } else {
      auto tensor_sizes = arg.sizes().vec();
      const auto num_chunks = chunk_desc.nSubTensors();
      const auto dim = at::maybe_wrap_dim(chunk_desc.dim(), tensor_sizes.size());
      if (tensor_sizes[dim] % num_chunks != 0) {
        return c10::nullopt;
      }
      tensor_sizes[dim] /= num_chunks;
      try {
        map_size = at::infer_size(map_size, tensor_sizes);
      } catch (...) {
        return c10::nullopt;
      }
    }
  }

  return {map_size};
}

static c10::optional<std::vector<int64_t>> canRunKernel(
  const KernelSpec& spec
, at::TensorList args) {
  // Short-circuits on size mismath
  AT_CHECK(args.size() == spec.inputChunks().size(),
           "Expected ", spec.inputChunks().size(), " arguments, but got ", args.size());

  c10::optional<std::vector<int64_t>> map_size;
  for (const auto& broadcast_group : spec.inputBroadcastGroups()) {
    if (!map_size) {
      map_size = getMapSize(spec, args, broadcast_group);
      if (!map_size) {
        return c10::nullopt;
      }
    } else {
      auto group_map_size = getMapSize(spec, args, broadcast_group);
      // NB: this checks that group_map_size is defined AND equal to map_size
      if (map_size != group_map_size) {
        return c10::nullopt;
      }
    }
  }

  return map_size;
}

// Note: Arguments are mutated by this call, although map_size is restored
// to its original value.
static void expandArgs(
  const KernelSpec& spec
, std::vector<at::Tensor>& args
, std::vector<int64_t>& map_size) {
  for (size_t i = 0; i < args.size(); ++i) {
    auto& arg = args[i];
    const auto& pdesc = spec.inputChunks()[i];
    if (pdesc.nSubTensors() == 1) {
      if (arg.sizes().equals(map_size)) continue;
      arg = arg.expand(map_size);
    } else {
      map_size.at(pdesc.dim()) *= pdesc.nSubTensors();
      if (!arg.sizes().equals(map_size)) {
        arg = arg.expand(map_size);
      }
      map_size.at(pdesc.dim()) /= pdesc.nSubTensors();
    }
  }
}

// Note: assumes that inputs are 32-bit addressable
static uint32_t computeNumel(const at::ArrayRef<int64_t>& sizes) {
  uint32_t result = 1;

  // Short-circuits if scalar tensor
  if (sizes.size() == 0) return 1;
  
  for (const auto& size : sizes) 
    result *= size;
  return result;
}

// Note: Assumes that after at::chunk, all inputs are the same size
static std::vector<int64_t> computeMapSize(
  const at::Tensor& tensor
, const PartitionDesc& chunkDesc) {
  std::vector<int64_t> sizes(tensor.sizes().begin(), tensor.sizes().end());
  // Should have been checked in graph fuser
  JIT_ASSERT(sizes[chunkDesc.dim] % chunkDesc.nSubtensors == 0);
  sizes[chunkDesc.dim] /= chunkDesc.nSubtensors;
  return sizes;
}

// Tries to compress sizes and strides according to cont. Emits the result t
// c_sizes, c_strides and throws an error on failure (if can't compress)
static void compressContiguous(
  const at::IntList& sizes
, const at::IntList& strides
, const std::vector<bool>& cont
, uint32_t* c_sizes
, uint32_t* c_strides) {
  size_t compressed_dims = 0;
  size_t cur = 0;
  size_t ndim = sizes.size();
  while (cur < ndim) {
    size_t total_size = sizes[cur];
    cur++;
    while (cont[cur-1] && cur < ndim) {
      JIT_ASSERT(strides[cur-1] == sizes[cur]*strides[cur]);
      total_size *= sizes[cur];
      cur++;
    }
    c_sizes[compressed_dims] = total_size;
    c_strides[compressed_dims] = strides[cur-1];
    compressed_dims++;
  }
  if (ndim > 0) {
    JIT_ASSERT(!cont.back() || strides.back() == 1);
  }
}

void launchFusion(
  const FusedKernel& fusion
, const int device
, const at::ArrayRef<at::Tensor>& inputs
, std::vector<at::Tensor>& outputs) {
  // Switches to device to run the fusion on
  at::DeviceGuard guard{device};
     
  // Allocates tensors for outputs
  auto& ref_type = inputs[0].type();
  outputs.reserve(fusion.output_desc_.size());
  for (const auto& od : fusion.output_desc_) {
    outputs.push_back(at::empty({0}, ref_type.options().dtype(od.scalar_type)));
  }

  // Fails if fusion and given inputs disagree
  JIT_ASSERT(inputs.size() == fusion.input_desc_.size());

  // Computes number of flattened inputs and outputs
  size_t flat_inputs_size = 0;
  size_t flat_outputs_size = 0;
  for (const auto& c : fusion.chunk_desc_)
    flat_inputs_size += c.nSubtensors;
  for (const auto& c : fusion.concat_desc_)
    flat_outputs_size += c.nSubtensors;
  
  // Fails if the elements of the first (any) tensor are not expressable as 
  // a 32-bit integer.
  // Note: this code assumes that inputs are 32-bit addressable
  // Note: this code assumes that all inputs are of the same size
  JIT_ASSERT(inputs[0].numel() <= std::numeric_limits<uint32_t>::max());

  // Computes map_size, numel from the first input
  at::IntList map_size;
  uint32_t numel;
  std::vector<int64_t> keep_alive_size;
  if (fusion.chunk_desc_[0].isNoop()) {
    map_size = inputs[0].sizes();
    numel = inputs[0].numel();
  } else {
    keep_alive_size = computeMapSize(inputs[0], fusion.chunk_desc_[0]);
    map_size = keep_alive_size;
    numel = computeNumel(map_size);
  }

  // Computes the storage needed to store TensorInfo structs for inputs and outputs.
  size_t uncompressedDim = fusion.input_desc_.at(0).contiguity.size();
  size_t maxPossibleTensorInfoSize = sizeof(TensorInfo) + 2 * sizeof(uint32_t) * uncompressedDim;
  size_t maxPossibleBufferSize = maxPossibleTensorInfoSize * (flat_inputs_size + flat_outputs_size);
  std::vector<char> buffer(maxPossibleBufferSize);
  char* buffer_next = buffer.data();

  // A vector of arguments to the kernel (numel, *input_desc_s, *output_desc_s)
  std::vector<void*> arguments;
  arguments.reserve(3 + flat_inputs_size + flat_outputs_size);
  auto addTensorInfoRaw = [&](
    const TensorDesc& desc
  , void* data_ptr
  , at::IntList sizes
  , at::IntList strides) {
    const auto nDim = desc.nDim(); // NOTE: this is the compressed dim
    JIT_ASSERT(nDim <= uncompressedDim); // We'd overflow the space otherwise
    auto ti = reinterpret_cast<TensorInfo*>(buffer_next);
    ti->data = data_ptr;
    compressContiguous(
      sizes
    , strides
    , desc.contiguity
    , ti->sizes(nDim)
    , ti->strides(nDim));
    buffer_next += maxPossibleTensorInfoSize;
    arguments.push_back(ti);
  };

  // Asserts that t's dims can be compressed in the same way as in desc
  // (that's what the kernel assumes), and appends it to the arguments vector.
  auto addTensorInfo = [&](const TensorDesc& desc, const at::Tensor& t) {
    addTensorInfoRaw(desc, t.data_ptr(), t.sizes(), t.strides());
  };

  arguments.push_back(&numel);
  for (size_t i = 0; i < fusion.input_desc_.size(); ++i) {
    const auto& chunk = fusion.chunk_desc_[i];
    const at::Tensor& tensor = inputs[i];
    if (chunk.isNoop()) {
      addTensorInfo(fusion.input_desc_[i], tensor);
    } else {
      size_t chunk_offset = map_size[chunk.dim] * tensor.stride(chunk.dim) * elementSize(tensor.type().scalarType());
      char* data_ptr = reinterpret_cast<char*>(tensor.data_ptr());
      for (size_t chunks = 0; chunks < chunk.nSubtensors; ++chunks) {
        addTensorInfoRaw(*chunk.subtensorDesc, data_ptr, map_size, tensor.strides());
        data_ptr += chunk_offset;
      }
    }
  }
  for (size_t i = 0; i < fusion.output_desc_.size(); ++i) {
    const auto& c = fusion.concat_desc_[i];
    auto& o = outputs[i];
    if (c.isNoop()) {
      o.resize_(map_size);
      addTensorInfo(fusion.output_desc_[i], outputs[i]);
    } else {
      size_t small_size = map_size[c.dim];
      std::vector<int64_t> concat_size(map_size.begin(), map_size.end());
      concat_size[c.dim] = small_size * c.nSubtensors;
      o.resize_(concat_size);
      size_t offset = 0;
      for (size_t j = 0; j < c.nSubtensors; ++j) {
        // because the concatenated_output stays live, the underlying data
        // in this view remains live through the end of this function
        // so there is not need to hold onto this tensor
        const auto view = o.narrow(c.dim, offset, small_size);
        addTensorInfo(*c.subtensorDesc, view);
        offset += small_size;
      }
    }
  }

  fusion.launch_raw(numel, arguments);
}


void runFusion(
  const int64_t key
, Stack& stack) {
  // Short-circuits if fusion isn't enabled
  if (!canFuseOnCPU() && !canFuseOnGPU())
    throw std::runtime_error("Fusion not enabled.");

  // Acquires the FusionSpec
  auto maybe_spec = retrieve(key);
  if (!maybe_spec) 
    throw std::runtime_error("Failed to find fusion specification to run.");
  auto& spec = *maybe_spec;
  
  // Short-circuits if the spec isn't fusable
  if (!spec.isFusable()) 
    throw std::runtime_error("Non-fusable specification.");

  // Determines device to dispatch to
  // Acquires inputs from stack
  auto inputs = fmap(last(stack, spec.nInputs()), [](const IValue& i) {
    return i.toTensor();
  });
  int32_t device = kCPUDevice;
  for (const auto& t : inputs) {
    const auto cur_device = t.device().index();
    if (cur_device < 0) continue;
    if (device < 0) device = cur_device;
    else if (device != cur_device) 
      throw std::runtime_error("Cannot fuse CUDA tensors on different devices.");
  }

  // Validates sizes and expands inputs as needed
  auto maybe_map_size = canRunKernel(spec, inputs);
  if (!maybe_map_size)
    throw std::runtime_error("Incompatible map size preventing fusion.");
  expandArgs(spec, inputs, *maybe_map_size);

  // Retrieves the kernel, compiling if necessary
  ArgSpec arg_spec{inputs};
  auto maybe_kernel = spec.findKernel(arg_spec);
  if (!maybe_kernel) {
    const auto kernel = compileKernel(spec, arg_spec, *maybe_map_size, device);
    spec.cacheKernel(arg_spec, kernel);
  }
  maybe_kernel = spec.findKernel(arg_spec);
  if (!maybe_kernel)
    throw std::runtime_error("Failed to find cached fused kernel.");

  // Launches fusion
  std::vector<at::Tensor> outputs;
  launchFusion(*(*maybe_kernel), device, inputs, outputs);

  // Updates stack
  drop(stack, spec.nInputs());
  stack.insert(
    stack.end()
  , std::make_move_iterator(outputs.begin())
  , std::make_move_iterator(outputs.end()));
}

} // namespace fuser
} // namespace jit
} // namespace torch
