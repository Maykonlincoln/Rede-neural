#include <ATen/ATen.h>
#include <ATen/cuda/CUDAApplyUtils.cuh>
#include <ATen/cuda/CUDAContext.h>
#include <ATen/NativeFunctions.h>
#include <ATen/TensorUtils.h>
#include <ATen/Utils.h>
// keeping THC headers for atomicAdd
#include <THC/THCGeneral.h>

namespace at {
namespace native {
namespace {

using at::cuda::detail::canUse32BitIndexMath;

__device__
inline void get_index_mapping(
    int64_t input_dim_x, int64_t input_dim_y,
    int64_t output_dim_x, int64_t output_dim_y,
    int64_t pad_l, int64_t pad_t,
    int64_t output_xy,
    int64_t & input_idx, int64_t & output_idx) {
  // 3D grid of 1D blocks
  auto input_offset =
    (blockIdx.y + blockIdx.z * gridDim.y) * input_dim_x * input_dim_y;
  auto output_offset =
    (blockIdx.y + blockIdx.z * gridDim.y) * output_dim_x * output_dim_y;

  auto output_x = output_xy % output_dim_x;
  auto output_y = output_xy / output_dim_x;

  auto i_start_x = ::max(0L, (long)-pad_l);
  auto i_start_y = ::max(0L, (long)-pad_t);
  auto o_start_x = ::max(0L, (long)pad_l);
  auto o_start_y = ::max(0L, (long)pad_t);

  auto input_x = ::abs(output_x - pad_l)
                 - ::abs(output_x - (input_dim_x + pad_l - 1))
                 - output_x
                 + 2 * pad_l + input_dim_x - 1
                 - o_start_x + i_start_x;

  auto input_y = ::abs(output_y - pad_t)
                 - ::abs(output_y - (input_dim_y + pad_t - 1))
                 - output_y
                 + 2 * pad_t + input_dim_y - 1
                 - o_start_y + i_start_y;

  input_idx = input_offset + input_y * input_dim_x + input_x;
  output_idx = output_offset + output_y * output_dim_x + output_x;
}

template<typename scalar_t>
__global__ void reflection_pad2d_out_kernel(
    scalar_t * input, scalar_t * output,
    int64_t input_dim_x, int64_t input_dim_y,
    int pad_t, int pad_b, int pad_l, int pad_r) {
  auto output_xy = threadIdx.x + blockIdx.x * blockDim.x;
  auto output_dim_x = input_dim_x + pad_l + pad_r;
  auto output_dim_y = input_dim_y + pad_t + pad_b;

  if (output_xy < output_dim_x * output_dim_y) {
    int64_t input_idx, output_idx;

    get_index_mapping(
      input_dim_x, input_dim_y,
      output_dim_x, output_dim_y,
      pad_l, pad_t,
      output_xy,
      input_idx, output_idx);

    output[output_idx] = input[input_idx];
  }
}

template <typename scalar_t>
__global__ void reflection_pad2d_backward_out_kernel(
    scalar_t * grad_input, scalar_t * grad_output,
    int64_t input_dim_x, int64_t input_dim_y,
    int pad_t, int pad_b, int pad_l, int pad_r) {
  auto output_xy = threadIdx.x + blockIdx.x * blockDim.x;
  auto output_dim_x = input_dim_x + pad_l + pad_r;
  auto output_dim_y = input_dim_y + pad_t + pad_b;

  if (output_xy < output_dim_x * output_dim_y) {
    int64_t input_idx, output_idx;

    get_index_mapping(
      input_dim_x, input_dim_y,
      output_dim_x, output_dim_y,
      pad_l, pad_t,
      output_xy,
      input_idx, output_idx);

    atomicAdd(&grad_input[input_idx], grad_output[output_idx]);
  }
}

void reflection_pad2d_out_template(
    Tensor &output, const Tensor &input_, IntList padding) {
  AT_CHECK(canUse32BitIndexMath(input_),
    "input tensor must fit into 32-bit index math");

  int plane_dim = 0;
  int dim_h = 1;
  int dim_w = 2;
  int nbatch = 1;

  AT_CHECK(input_.numel() > 0 &&
    (input_.ndimension() == 3 || input_.ndimension() == 4), "non-empty 3D or "
    "4D (batch mode) tensor expected for input, but got: ", input_);

  if (input_.ndimension() == 4) {
    nbatch = input_.size(0);
    plane_dim++;
    dim_h++;
    dim_w++;
  }

  int64_t pad_l = padding[0];
  int64_t pad_r = padding[1];
  int64_t pad_t = padding[2];
  int64_t pad_b = padding[3];

  int nplane = input_.size(plane_dim);
  int input_h = input_.size(dim_h);
  int input_w = input_.size(dim_w);

  AT_CHECK(pad_l < input_w && pad_r < input_w,
    "Padding size should be less than the corresponding input dimension, but "
    "got: padding (", pad_l, ", ", pad_r, ") at dimension ", dim_w,
    " of input ", input_.sizes());

  AT_CHECK(pad_t < input_h && pad_b < input_h,
    "Padding size should be less than the corresponding input dimension, but "
    "got: padding (", pad_t, ", ", pad_b, ") at dimension ", dim_h,
    " of input ", input_.sizes());

  int output_h = input_h + pad_t + pad_b;
  int output_w  = input_w + pad_l + pad_r;

  AT_CHECK(output_w >= 1 || output_h >= 1,
    "input (H: ", input_h, ", W: ", input_w, ")is too small.  Calculated "
    "output H: ", output_h, " W: ", output_w);

  if (input_.ndimension() == 3) {
    output.resize_({nplane, output_h, output_w});
  } else {
    output.resize_({nbatch, nplane, output_h, output_w});
  }

  Tensor input = input_.contiguous();

  int output_plane_size = output_h * output_w;
  dim3 block_size(output_plane_size > 256 ? 256 : output_plane_size);
  dim3 grid_size(
    (int) std::ceil(output_plane_size/256.0), nplane, nbatch);

  AT_DISPATCH_FLOATING_TYPES_AND_HALF(
    input.type(), "reflection_pad2d_out_template", [&] {
      reflection_pad2d_out_kernel<<<
        grid_size, block_size, 0, at::cuda::getCurrentCUDAStream()>>>(
          input.data<scalar_t>(), output.data<scalar_t>(),
          input_w, input_h,
          pad_t, pad_b, pad_l, pad_r);
    }
  );

  AT_CUDA_CHECK(cudaGetLastError());
}

void reflection_pad2d_backward_out_template(
    Tensor &grad_input, const Tensor &grad_output_,
    const Tensor &input, IntList padding) {
  AT_CHECK(canUse32BitIndexMath(input),
    "input tensor must fit into 32-bit index math");
  AT_CHECK(canUse32BitIndexMath(grad_output_),
    "output gradient tensor must fit into 32-bit index math");

  int plane_dim = 0;
  int dim_h = 1;
  int dim_w = 2;
  int nbatch = 1;

  if (input.ndimension() == 4) {
    nbatch = input.size(0);
    plane_dim++;
    dim_h++;
    dim_w++;
  }

  int64_t pad_l = padding[0];
  int64_t pad_r = padding[1];
  int64_t pad_t = padding[2];
  int64_t pad_b = padding[3];

  int nplane = input.size(plane_dim);
  int input_h = input.size(dim_h);
  int input_w = input.size(dim_w);

  int output_h = input_h + pad_t + pad_b;
  int output_w  = input_w + pad_l + pad_r;

  AT_CHECK(output_w == grad_output_.size(dim_w), "grad_output width "
    "unexpected. Expected: ", output_w, ", Got: ", grad_output_.size(dim_w));
  AT_CHECK(output_h == grad_output_.size(dim_h), "grad_output height "
    "unexpected. Expected: ", output_h, ", Got: ", grad_output_.size(dim_h));

  Tensor grad_output = grad_output_.contiguous();

  int output_plane_size = output_h * output_w;
  dim3 block_size(output_plane_size > 256 ? 256 : output_plane_size);
  dim3 grid_size(
    (int) std::ceil(output_plane_size/256.0), nplane, nbatch);

  AT_DISPATCH_FLOATING_TYPES_AND_HALF(
    input.type(), "reflection_pad2d_backward_out_template", [&] {
      reflection_pad2d_backward_out_kernel<<<
        grid_size, block_size, 0, at::cuda::getCurrentCUDAStream()>>>(
          grad_input.data<scalar_t>(), grad_output.data<scalar_t>(),
          input_w, input_h,
          pad_t, pad_b, pad_l, pad_r);
    }
  );

  AT_CUDA_CHECK(cudaGetLastError());
}

} // namespace

Tensor& reflection_pad2d_out_cuda(
    Tensor& output, const Tensor& input, IntList padding) {
  reflection_pad2d_out_template(output, input, padding);
  return output;
}

Tensor reflection_pad2d_cuda(Tensor const& input, IntList padding) {
  auto output = at::empty({0}, input.options());
  reflection_pad2d_out_template(output, input, padding);
  return output;
}

Tensor& reflection_pad2d_backward_out_cuda(
    Tensor& grad_input, const Tensor& grad_output,
    const Tensor& input,
    IntList padding) {
  grad_input = at::zeros_like(input);
  reflection_pad2d_backward_out_template(
    grad_input, grad_output, input, padding);
  return grad_input;
}

Tensor reflection_pad2d_backward_cuda(
    const Tensor& grad_output,
    const Tensor& input,
    IntList padding) {
  auto grad_input = at::zeros_like(input);
  reflection_pad2d_backward_out_template(
    grad_input, grad_output, input, padding);
  return grad_input;
}

} // namespace native
} // namespace at
