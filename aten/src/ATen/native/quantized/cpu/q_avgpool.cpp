#include <ATen/ATen.h>
#include <ATen/NativeFunctions.h>
#include <ATen/Parallel.h>
#include <ATen/native/Pool.h>
#include <ATen/native/quantized/cpu/init_qnnpack.h>
#include <ATen/native/quantized/cpu/qnnpack_utils.h>
#include <ATen/native/quantized/cpu/quantized_ops.h>
#include <caffe2/utils/threadpool/ThreadPoolMobile.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace at {
namespace native {
namespace {

DEFINE_DISPATCH(qavg_pool2d_nhwc_stub);

template <typename scalar_t>
static void avg_pool2d_out_frame(
    const Tensor& input,
    Tensor& output,
    int64_t b,
    int64_t nInputPlane,
    int64_t inputWidth,
    int64_t inputHeight,
    int64_t outputWidth,
    int64_t outputHeight,
    int kW,
    int kH,
    int dW,
    int dH,
    int padW,
    int padH,
    bool count_include_pad,
    c10::optional<int64_t> divisor_override) {
  at::parallel_for(0, nInputPlane, 0, [&](int64_t start, int64_t end) {
    for (auto k = start; k < end; k++) {
      int64_t xx, yy;
      /* For all output pixels... */
      auto input_data = input.contiguous().data_ptr<scalar_t>();
      auto output_data = output.data_ptr<scalar_t>();
      scalar_t* ptr_output = output_data +
          b * nInputPlane * outputWidth * outputHeight +
          k * outputWidth * outputHeight;
      const scalar_t* ptr_input = input_data +
          b * nInputPlane * inputWidth * inputHeight +
          k * inputWidth * inputHeight;
      auto minimum =
          std::numeric_limits<typename scalar_t::underlying>::lowest();
      auto maximum = std::numeric_limits<typename scalar_t::underlying>::max();

      for (yy = 0; yy < outputHeight; yy++) {
        for (xx = 0; xx < outputWidth; xx++) {
          /* Compute the mean of the input image... */
          int64_t hstart = yy * dH - padH;
          int64_t wstart = xx * dW - padW;
          int64_t hend = std::min(hstart + kH, inputHeight + padH);
          int64_t wend = std::min(wstart + kW, inputWidth + padW);
          int64_t pool_size = (hend - hstart) * (wend - wstart);
          hstart = std::max(hstart, (int64_t)0);
          wstart = std::max(wstart, (int64_t)0);
          hend = std::min(hend, inputHeight);
          wend = std::min(wend, inputWidth);

          int sum_int = 0;
          ptr_output->val_ = 0;

          int64_t divide_factor;
          int64_t size = (hend - hstart) * (wend - wstart);
          if (divisor_override.has_value()) {
            divide_factor = divisor_override.value();
          } else {
            if (count_include_pad) {
              divide_factor = pool_size;
            } else {
              divide_factor = (hend - hstart) * (wend - wstart);
            }
          }

          int64_t kx, ky;
          for (ky = hstart; ky < hend; ky++) {
            for (kx = wstart; kx < wend; kx++)
              sum_int += (ptr_input + ky * inputWidth + kx)->val_;
          }
          float multiplier = input.q_scale() / output.q_scale() / divide_factor;

          sum_int -= size * input.q_zero_point();
          float sum = sum_int * 1.0;
          /* Update output by requantizing the result */
          ptr_output->val_ =
              static_cast<typename scalar_t::underlying>(std::min<int32_t>(
                  std::max<int32_t>(
                      std::nearbyint(sum * multiplier + output.q_zero_point()),
                      minimum),
                  maximum));
          std::cout << "Value is " << sum * multiplier + output.q_zero_point() << std::endl;
          ptr_output++;
        }
      }
    }
  });
}

inline std::pair<int, int> get_kernel(IntArrayRef kernel_size) {
  TORCH_CHECK(
      kernel_size.size() == 1 || kernel_size.size() == 2,
      "avg_pool2d: kernel_size must either be a single int, or a tuple of two ints");
  const int kH = safe_downcast<int, int64_t>(kernel_size[0]);
  const int kW = kernel_size.size() == 1
      ? kH
      : safe_downcast<int, int64_t>(kernel_size[1]);
  return std::make_pair(kW, kH);
}

inline std::pair<int, int> get_stride(IntArrayRef stride, int kW, int kH) {
  TORCH_CHECK(
      stride.empty() || stride.size() == 1 || stride.size() == 2,
      "avg_pool2d: stride must either be omitted, a single int, or a tuple of two ints");
  const int dH = stride.empty() ? kH : safe_downcast<int, int64_t>(stride[0]);
  const int dW = stride.empty()
      ? kW
      : stride.size() == 1 ? dH : safe_downcast<int, int64_t>(stride[1]);
  return std::make_pair(dW, dH);
}

inline std::pair<int, int> get_padding(IntArrayRef padding) {
  TORCH_CHECK(
      padding.size() == 1 || padding.size() == 2,
      "avg_pool2d: padding must either be a single int, or a tuple of two ints");
  const int padH = safe_downcast<int, int64_t>(padding[0]);
  const int padW =
      padding.size() == 1 ? padH : safe_downcast<int, int64_t>(padding[1]);
  return std::make_pair(padW, padH);
}

std::vector<int64_t> get_output_shape(
    const Tensor& input_,
    int kW,
    int kH,
    int dW,
    int dH,
    int padW,
    int padH,
    bool ceil_mode) {
  const int64_t nbatch = input_.ndimension() == 4 ? input_.size(-4) : 1;
  const int64_t nInputPlane = input_.size(-3);
  const int64_t inputHeight = input_.size(-2);
  const int64_t inputWidth = input_.size(-1);
  const int64_t outputHeight =
      pooling_output_shape<int64_t>(inputHeight, kH, padH, dH, 1, ceil_mode);
  const int64_t outputWidth =
      pooling_output_shape<int64_t>(inputWidth, kW, padW, dW, 1, ceil_mode);
  if (input_.ndimension() == 3) {
    return {nInputPlane, outputHeight, outputWidth};
  }
  return {nbatch, nInputPlane, outputHeight, outputWidth};
}

template <typename scalar_t>
Tensor q_avg_pool2d(
    const Tensor& input,
    IntArrayRef kernel_size,
    IntArrayRef stride,
    IntArrayRef padding,
    bool ceil_mode,
    bool count_include_pad,
    c10::optional<int64_t> divisor_override) {
  int kW, kH, dW, dH, padW, padH;
  std::tie(kW, kH) = get_kernel(kernel_size);
  std::tie(dW, dH) = get_stride(stride, kW, kH);
  std::tie(padW, padH) = get_padding(padding);

  const int64_t nbatch = input.ndimension() == 4 ? input.size(-4) : 1;
  const int64_t nInputPlane = input.size(-3);
  const int64_t inputHeight = input.size(-2);
  const int64_t inputWidth = input.size(-1);

  TORCH_CHECK(
      !divisor_override.has_value() || divisor_override.value() != 0,
      "divisor must be not zero");

  auto output_shape =
      get_output_shape(input, kW, kH, dW, dH, padW, padH, ceil_mode);
  const int64_t outputHeight = output_shape[output_shape.size() - 2];
  const int64_t outputWidth = output_shape[output_shape.size() - 1];
  if (input.is_contiguous(c10::MemoryFormat::ChannelsLast)) {
    auto output = at::_empty_affine_quantized(
        output_shape,
        input.options(),
        input.q_scale(),
        input.q_zero_point(),
        input.suggest_memory_format());
    // fast path for channel last: qavg_pool_2d_nhwc_stub
    if (output_shape.size() == 3) {
      qavg_pool2d_nhwc_stub(
          input.device().type(),
          input,
          output,
          0,
          nInputPlane,
          inputWidth,
          inputHeight,
          outputWidth,
          outputHeight,
          kW,
          kH,
          dW,
          dH,
          padW,
          padH,
          count_include_pad,
          divisor_override);
    } else {
      at::parallel_for(0, nbatch, 0, [&](int64_t start, int64_t end) {
        for (auto b = start; b < end; b++) {
          qavg_pool2d_nhwc_stub(
              input.device().type(),
              input,
              output,
              b,
              nInputPlane,
              inputWidth,
              inputHeight,
              outputWidth,
              outputHeight,
              kW,
              kH,
              dW,
              dH,
              padW,
              padH,
              count_include_pad,
              divisor_override);
        }
      });
    }
    return output;
  } else {
    auto output = at::_empty_affine_quantized(
        output_shape, input.options(), input.q_scale(), input.q_zero_point());
    if (output_shape.size() == 3) {
      avg_pool2d_out_frame<scalar_t>(
          input,
          output,
          0,
          nInputPlane,
          inputWidth,
          inputHeight,
          outputWidth,
          outputHeight,
          kW,
          kH,
          dW,
          dH,
          padW,
          padH,
          count_include_pad,
          divisor_override);
    } else {
      at::parallel_for(0, nbatch, 0, [&](int64_t start, int64_t end) {
        for (auto b = start; b < end; b++) {
          avg_pool2d_out_frame<scalar_t>(
              input,
              output,
              b,
              nInputPlane,
              inputWidth,
              inputHeight,
              outputWidth,
              outputHeight,
              kW,
              kH,
              dW,
              dH,
              padW,
              padH,
              count_include_pad,
              divisor_override);
        }
      });
    }
    return output;
  }
}
#ifdef USE_PYTORCH_QNNPACK
Tensor qnnpack_avg_pool2d(
    Tensor input,
    IntArrayRef kernel_size,
    IntArrayRef stride,
    IntArrayRef padding,
    bool ceil_mode,
    bool count_include_pad,
    c10::optional<int64_t> divisor_override) {
  Tensor output;
  int kW, kH, dW, dH, padW, padH;
  std::tie(kW, kH) = get_kernel(kernel_size);
  std::tie(dW, dH) = get_stride(stride, kW, kH);
  std::tie(padW, padH) = get_padding(padding);
  TORCH_CHECK(
      input.ndimension() == 4,
      "qnnpack_avg_pool2d(): Expected input to be 4-dimensional: got ",
      input.ndimension());

  int64_t batch_size = input.size(0);
  int64_t inC = input.size(1);
  int64_t inH = input.size(2);
  int64_t inW = input.size(3);
  auto output_shape =
      get_output_shape(input, kW, kH, dW, dH, padW, padH, ceil_mode);
  const int64_t oH = output_shape[output_shape.size() - 2];
  const int64_t oW = output_shape[output_shape.size() - 1];
  const auto outC = inC;

  Tensor input_contig = input.permute({0, 2, 3, 1}).contiguous();

  initQNNPACK();
  const auto scale = input_contig.q_scale();
  const auto zero_point = input_contig.q_zero_point();

  // TODO fix the kernels. Currently it appears that if input zero point and
  // padding are non-zero, qnnpack does not treat the extra padded values as 0,
  // but possibly scales them based on input_zero_point, producing incorrect results.
  TORCH_CHECK(
      padW == 0 && padH == 0, "qnnpack_avg_pool2d(): currently does "
      "not support non-zero padding");
  TORCH_CHECK(
      oH > 0 && oW > 0,
      "qnnpack_avg_pool2d(): the resulting output Tensor size should be >= 0");
  // NHWC output
  output = at::_empty_affine_quantized(
      {batch_size, oH, oW, outC},
      at::device(kCPU).dtype(kQUInt8),
      scale,
      zero_point);

  pytorch_qnnp_operator_t qnnpack_operator{nullptr};
  const pytorch_qnnp_status createStatus =
      pytorch_qnnp_create_average_pooling2d_nhwc_q8(
          padH /* input_padding_top */,
          padW /* input_padding_right */,
          padH /* input_padding_bottom */,
          padW /* input_padding_left */,
          kH /* kernel height */,
          kW /* kernel width */,
          dH /* stride height */,
          dW /* stride width */,
          inC /* input channels */,
          zero_point /* input zero_point */,
          scale /* input scale */,
          zero_point /* output zero_point */,
          scale /* output scale */,
          std::numeric_limits<uint8_t>::min() /* output min */,
          std::numeric_limits<uint8_t>::max() /* output max */,
          0 /* flags */,
          &qnnpack_operator);
  CAFFE_ENFORCE(
      createStatus == pytorch_qnnp_status_success,
      "failed to create QNNPACK Average Pooling operator");
  std::unique_ptr<pytorch_qnnp_operator, QnnpackOperatorDeleter>
      qnnpack_uniq_ptr(qnnpack_operator);

  const pytorch_qnnp_status setupStatus =
      pytorch_qnnp_setup_average_pooling2d_nhwc_q8(
          qnnpack_operator,
          batch_size,
          inH,
          inW,
          (uint8_t*)input_contig.data_ptr<c10::quint8>() /* input data */,
          inC,
          (uint8_t*)output.data_ptr<c10::quint8>() /* output data */,
          outC,
          nullptr /* thread pool */);
  CAFFE_ENFORCE(
      setupStatus == pytorch_qnnp_status_success,
      "failed to setup QNNPACK Average Pooling operator");
  pthreadpool_t threadpool = caffe2::mobile_pthreadpool();
  const pytorch_qnnp_status runStatus =
      pytorch_qnnp_run_operator(qnnpack_operator, threadpool);
  TORCH_INTERNAL_ASSERT(
      runStatus == pytorch_qnnp_status_success,
      "failed to run QNNPACK Average Pool operator");
  // TODO: remove permute once MemoryLayout is added above
  return output.permute({0, 3, 1, 2});
}
#endif
} // namespace

Tensor quantized_avg_pool2d(
    const Tensor& input,
    IntArrayRef kernel_size,
    IntArrayRef stride,
    IntArrayRef padding,
    bool ceil_mode,
    bool count_include_pad,
    c10::optional<int64_t> divisor_override) {
  Tensor output;
#ifdef USE_PYTORCH_QNNPACK
  if (at::globalContext().qEngine() == at::QEngine::QNNPACK &&
      input.scalar_type() == kQUInt8) {
    return qnnpack_avg_pool2d(
        input,
        kernel_size,
        stride,
        padding,
        ceil_mode,
        count_include_pad,
        divisor_override);
  }
#endif
  AT_DISPATCH_QINT_TYPES(input.scalar_type(), "quantized_avg_pool2d", [&]() {
    output = q_avg_pool2d<scalar_t>(
        input,
        kernel_size,
        stride,
        padding,
        ceil_mode,
        count_include_pad,
        divisor_override);
  });
  return output;
}

} // namespace native
} // namespace at
