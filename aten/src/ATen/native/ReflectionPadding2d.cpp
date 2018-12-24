#include <ATen/ATen.h>
#include <ATen/NativeFunctions.h>
#include <tuple>

namespace at {
namespace native {

namespace {

template <typename scalar_t>
static void reflection_pad2d_out_frame(
    scalar_t * input_p, scalar_t * output_p,
    int64_t nplane,
    int64_t input_w, int64_t input_h,
    int64_t output_w, int64_t output_h,
    int64_t pad_l, int64_t pad_t) {
  auto i_start_x = std::max(0L, (long)-pad_l);
  auto i_start_y = std::max(0L, (long)-pad_t);
  auto o_start_x = std::max(0L, (long)pad_l);
  auto o_start_y = std::max(0L, (long)pad_t);

  int64_t k, ip_x, ip_y;
#pragma omp parallel for private(k, ip_x, ip_y)

  for (k = 0; k < nplane; k++) {
    for (int64_t i = 0; i < output_h; i++) {
      for (int64_t j = 0; j < output_w; j++) {
        if (j < pad_l) {
          ip_x = pad_l * 2 - j;
        } else if (j >= pad_l && j < input_w + pad_l) {
          ip_x = j;
        } else {
          ip_x = (input_w + pad_l - 1) * 2 - j;
        }
        ip_x = ip_x - o_start_x + i_start_x;

        if (i < pad_t) {
          ip_y = pad_t * 2 - i;
        } else if (i >= pad_t && i < input_h + pad_t) {
          ip_y = i;
        } else {
          ip_y = (input_h + pad_t - 1) * 2 - i;
        }
        ip_y = ip_y - o_start_y + i_start_y;

        scalar_t *dest_p = output_p + k*output_w*output_h + i * output_w + j;
        scalar_t *src_p = input_p + k*input_w*input_h + ip_y * input_w + ip_x;
        *dest_p = *src_p;
      }
    }
  }
}

void reflection_pad2d_out_template(
    Tensor &output, const Tensor &input_, IntList padding) {
  int dim_w = 2;
  int dim_h = 1;
  int dim_slices = 0;
  int64_t nbatch = 1;

  AT_CHECK(input_.numel() > 0 &&
    (input_.ndimension() == 3 || input_.ndimension() == 4), "non-empty 3D or "
    "4D (batch mode) tensor expected for input, but got: ", input_);

  if (input_.ndimension() == 4) {
    nbatch = input_.size(0);
    dim_w++;
    dim_h++;
    dim_slices++;
  }

  /* sizes */
  int64_t pad_l = padding[0];
  int64_t pad_r = padding[1];
  int64_t pad_t = padding[2];
  int64_t pad_b = padding[3];

  int64_t nplane = input_.size(dim_slices);
  int64_t input_h = input_.size(dim_h);
  int64_t input_w = input_.size(dim_w);
  int64_t output_h = input_h + pad_t + pad_b;
  int64_t output_w  = input_w + pad_l + pad_r;

  AT_CHECK(pad_l < input_w && pad_r < input_w,
    "Argument #4: Padding size should be less than the corresponding "
    "input dimension, but got: padding (", pad_l, ", ", pad_r,
    ") at dimension ", dim_w, " of input ", input_.ndimension());

  AT_CHECK(pad_t < input_h && pad_b < input_h,
    "Argument #6: Padding size should be less than the corresponding "
    "input dimension, but got: padding (", pad_t, ", ", pad_b,
    ") at dimension ", dim_h, " of input ", input_.ndimension());

  AT_CHECK(output_w >= 1 || output_h >= 1,
    "input (H: ", input_h, ", W: ", input_w, ")is too small. Calculated "
    "output H: ", output_h, " W: ", output_w);

  /* get contiguous input */
  Tensor input = input_.contiguous();

  if (input.ndimension() == 3) {
    /* resize output */
    output.resize_({nplane, output_h, output_w});

    AT_DISPATCH_FLOATING_TYPES_AND_HALF(input.type(), "reflection_pad2d", [&] {
      reflection_pad2d_out_frame(
        input.data<scalar_t>(), output.data<scalar_t>(),
        nplane,
        input_w, input_h, output_w, output_h,
        pad_l, pad_t);
    });
  } else {
    int64_t p;
    /* resize output */
    output.resize_({nbatch, nplane, output_h, output_w});

#pragma omp parallel for private(p)
    for (p = 0; p < nbatch; p++) {
      AT_DISPATCH_FLOATING_TYPES_AND_HALF(
        input.type(), "reflection_pad2d", [&] {
          reflection_pad2d_out_frame(
            input.data<scalar_t>() + p * nplane * input_w * input_h,
            output.data<scalar_t>() + p * nplane * output_w * output_h,
            nplane,
            input_w, input_h, output_w, output_h,
            pad_l, pad_t);
        }
      );
    }
  }
}

template <typename scalar_t>
static void reflection_pad2d_backward_out_frame(
    scalar_t *ginput_p, scalar_t *goutput_p,
    int64_t nplane,
    int64_t input_w, int64_t input_h,
    int64_t output_w, int64_t output_h,
    int64_t pad_l, int64_t pad_t) {
  auto i_start_x = std::max(0L, (long)-pad_l);
  auto i_start_y = std::max(0L, (long)-pad_t);
  auto o_start_x = std::max(0L, (long)pad_l);
  auto o_start_y = std::max(0L, (long)pad_t);

  int64_t k, ip_x, ip_y;
#pragma omp parallel for private(k, ip_x, ip_y)

  for (k = 0; k < nplane; k++) {
    for (int64_t i = 0; i < output_h; i++) {
      for (int64_t j = 0; j < output_w; j++) {
        if (j < pad_l) {
          ip_x = pad_l * 2 - j;
        } else if (j >= pad_l && j < input_w + pad_l) {
          ip_x = j;
        } else {
          ip_x = (input_w + pad_l - 1) * 2 - j;
        }
        ip_x = ip_x - o_start_x + i_start_x;

        if (i < pad_t) {
          ip_y = pad_t * 2 - i;
        } else if (i >= pad_t && i < input_h + pad_t) {
          ip_y = i;
        } else {
          ip_y = (input_h + pad_t - 1) * 2 - i;
        }
        ip_y = ip_y - o_start_y + i_start_y;

        scalar_t *src_p = goutput_p + k*output_w*output_h + i * output_w + j;
        scalar_t *dest_p = ginput_p + k*input_w*input_h + ip_y * input_w + ip_x;
        *dest_p += *src_p;
      }
    }
  }
}

void reflection_pad2d_backward_out_template(
    Tensor &grad_input, const Tensor &grad_output_,
    const Tensor &input, IntList padding) {
  int dim_w = 2;
  int dim_h = 1;
  int dim_plane = 0;
  int64_t nbatch = 1;

  if (input.ndimension() == 4) {
    nbatch = input.size(0);
    dim_w++;
    dim_h++;
    dim_plane++;
  }

  /* sizes */
  int64_t pad_l = padding[0];
  int64_t pad_r = padding[1];
  int64_t pad_t = padding[2];
  int64_t pad_b = padding[3];

  int64_t nplane = input.size(dim_plane);
  int64_t input_h = input.size(dim_h);
  int64_t input_w = input.size(dim_w);
  int64_t output_h = input_h + pad_t + pad_b;
  int64_t output_w  = input_w + pad_l + pad_r;

  AT_CHECK(output_w == grad_output_.size(dim_w),
    "gradOutput width unexpected. Expected: ", output_w, ", Got: ",
    grad_output_.size(dim_w));

  AT_CHECK(output_h == grad_output_.size(dim_h),
    "gradOutput height unexpected. Expected: ", output_h, ", Got: ",
    grad_output_.size(dim_h));

  /* get contiguous gradOutput */
  Tensor grad_output = grad_output_.contiguous();

  /* backprop */
  if (input.ndimension() == 3) {
    AT_DISPATCH_FLOATING_TYPES_AND_HALF(
      grad_output.type(), "reflection_pad2d_backward", [&] {
        reflection_pad2d_backward_out_frame(
          grad_input.data<scalar_t>(),
          grad_output.data<scalar_t>(),
          nplane,
          input_w, input_h, output_w, output_h,
          pad_l, pad_t);
      }
    );
  } else {
    int64_t p;
#pragma omp parallel for private(p)
    for (p = 0; p < nbatch; p++) {
      AT_DISPATCH_FLOATING_TYPES_AND_HALF(
        grad_output.type(), "reflection_pad2d_backward", [&] {
          reflection_pad2d_backward_out_frame(
            grad_input.data<scalar_t>() + p * nplane * input_h * input_w,
            grad_output.data<scalar_t>() + p * nplane * output_h * output_w,
            nplane,
            input_w, input_h, output_w, output_h,
            pad_l, pad_t);
        }
      );
    }
  }
}

} // namespace

Tensor& reflection_pad2d_out_cpu(
    Tensor& output, const Tensor& input, IntList padding) {
  reflection_pad2d_out_template(output, input, padding);
  return output;
}

Tensor reflection_pad2d_cpu(Tensor const& input, IntList padding) {
  auto output = at::empty({0}, input.options());
  reflection_pad2d_out_template(output, input, padding);
  return output;
}

Tensor& reflection_pad2d_backward_out_cpu(
    Tensor& grad_input,
    const Tensor& grad_output,
    const Tensor& input,
    IntList padding) {
  grad_input = at::zeros_like(input);
  reflection_pad2d_backward_out_template(
    grad_input, grad_output, input, padding);
  return grad_input;
}

Tensor reflection_pad2d_backward_cpu(
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
