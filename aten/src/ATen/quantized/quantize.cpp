#include <ATen/ATen.h>
#include <ATen/core/Tensor.h>
#include <ATen/quantized/quantize.h>

#ifdef USE_FBGEMM
#include <fbgemm/QuantUtils.h>
#endif

namespace at {

#ifdef USE_FBGEMM
// Note: quantize_val is only explicitly used in test outside of this file
template <typename T>
T quantize_val(double scale, int64_t zero_point, float value) {
  // Internally, fbgemm::Quantize uses std::nearbyint.
  // std::nearbyint results in nearest integer value according to the current
  // rounding mode and the default rounding mode is rounds to even in half-way
  // cases in most popular processor architectures like x86 and ARM. This is
  // typically faster than an alternatives like std::round that rounds half-way
  // cases away from zero, and can be consistent with SIMD implementations for
  // example in x86 using _mm512_cvtps_epi32 or mm512_round_ps with
  // _MM_FROUND_CUR_DIRECTION option that also follow the current rounding mode.
  int32_t qvalue;
  qvalue = fbgemm::Quantize<typename T::underlying>(
      value,
      static_cast<int32_t>(zero_point),
      static_cast<double>(scale),
      /*result_precision=*/CHAR_BIT * sizeof(typename T::underlying));
  return static_cast<T>(qvalue);
}

template <typename T, int precision>
void quantize_vec(double scale, int64_t zero_point, const float *src, T *dst, size_t count) {
  fbgemm::Quantize<typename T::underlying>(
    src,
    (typename T::underlying*)dst,
    count,
    fbgemm::TensorQuantizationParams{(float)scale, (int32_t)zero_point, precision}
  );
}

template <typename T>
inline float dequantize_val(double scale, int64_t zero_point, T value) {
  fbgemm::TensorQuantizationParams qparams;
  qparams.scale = static_cast<float>(scale);
  qparams.zero_point = static_cast<int32_t>(zero_point);
  return fbgemm::Dequantize<typename T::underlying>(value.val_, qparams);
}
#else  // USE_FBGEMM

#if defined(__ANDROID__) && !defined(__NDK_MAJOR__)
template <class T>
inline float Round(const float x) {
  return ::nearbyintf(x);
}
inline double Round(const double x) {
  return ::nearbyint(x);
}
#else
template <class T>
inline T Round(const T x) {
  return std::nearbyint(x);
}
#endif

template <typename T>
T quantize_val(double scale, int64_t zero_point, float value) {
  // std::nearbyint results in nearest integer value according to the current
  // rounding mode and the default rounding mode is rounds to even in half-way
  // cases in most popular processor architectures like x86 and ARM. This is
  // typically faster than an alternatives like std::round that rounds half-way
  // cases away from zero, and can be consistent with SIMD implementations for
  // example in x86 using _mm512_cvtps_epi32 or mm512_round_ps with
  // _MM_FROUND_CUR_DIRECTION option that also follow the current rounding mode.
  int64_t qvalue;
  constexpr int64_t qmin = std::numeric_limits<typename T::underlying>::min();
  constexpr int64_t qmax = std::numeric_limits<typename T::underlying>::max();
  qvalue = static_cast<int64_t>(Round(value / scale + zero_point));
  qvalue = std::max<int64_t>(qvalue, qmin);
  qvalue = std::min<int64_t>(qvalue, qmax);
  return static_cast<T>(qvalue);
}

template <typename T, int precision>
void quantize_vec(double scale, int64_t zero_point, const float *src, T *dst, size_t count) {
  checkZeroPoint<typename T::underlying>("quantize_val", zero_point);
  for (int64_t i = 0; i < count; ++i) {
    dst[i] = quantize_val<T>(scale, zero_point, src[i]);
  }
}

// TODO combine this with quantize_val once the numerics for ARM are aligned with it
inline uint8_t quantize_val_arm(const float scale, const int32_t zero_point, const float value) {
  const int32_t qmin = std::numeric_limits<uint8_t>::min();
  const int32_t qmax = std::numeric_limits<uint8_t>::max();
  auto r = zero_point + static_cast<int32_t>(Round(value / scale));
  r = std::max(r, qmin);
  r = std::min(r, qmax);
  return static_cast<uint8_t>(r);
}

template <typename T>
CAFFE2_API float dequantize_val(double scale, int64_t zero_point, T value) {
  // We need to convert the qint8 value to float to ensure the subtraction
  // subexpression returns a float
  return (static_cast<float>(value.val_) - zero_point) * scale;
}
#endif  // USE_FBGEMM

template <typename SRC_T, typename DST_T>
DST_T requantize_val(double src_scale, int64_t src_zero_point,
                     double dst_scale, int64_t dst_zero_point,
                     SRC_T src) {
  const auto dq = dequantize_val<SRC_T>(src_scale, src_zero_point, src);
  return quantize_val<DST_T>(dst_scale, dst_zero_point, dq);
}

template CAFFE2_API qint8 quantize_val<qint8>(double scale, int64_t zero_point, float value);
template CAFFE2_API quint8 quantize_val<quint8>(double scale, int64_t zero_point, float value);
template CAFFE2_API qint32 quantize_val<qint32>(double scale, int64_t zero_point, float value);
template CAFFE2_API void quantize_vec<c10::qint8>(double scale, int64_t zero_point, const float *src, c10::qint8 *dst, size_t count);
template CAFFE2_API void quantize_vec<c10::quint8>(double scale, int64_t zero_point, const float *src, c10::quint8 *dst, size_t count);
template CAFFE2_API void quantize_vec<c10::qint32, 32>(double scale, int64_t zero_point, const float *src, c10::qint32 *dst, size_t count);

template CAFFE2_API float dequantize_val<qint8>(double scale, int64_t zero_point, qint8 value);
template CAFFE2_API float dequantize_val<quint8>(double scale, int64_t zero_point, quint8 value);
template CAFFE2_API float dequantize_val<qint32>(double scale, int64_t zero_point, qint32 value);

template CAFFE2_API qint8 requantize_val<qint8, qint8>(double, int64_t, double, int64_t, qint8);
template CAFFE2_API quint8 requantize_val<qint8, quint8>(double, int64_t, double, int64_t, qint8);
template CAFFE2_API qint32 requantize_val<qint8, qint32>(double, int64_t, double, int64_t, qint8);
template CAFFE2_API qint8 requantize_val<quint8, qint8>(double, int64_t, double, int64_t, quint8);
template CAFFE2_API quint8 requantize_val<quint8, quint8>(double, int64_t, double, int64_t, quint8);
template CAFFE2_API qint32 requantize_val<quint8, qint32>(double, int64_t, double, int64_t, quint8);
template CAFFE2_API qint8 requantize_val<qint32, qint8>(double, int64_t, double, int64_t, qint32);
template CAFFE2_API quint8 requantize_val<qint32, quint8>(double, int64_t, double, int64_t, qint32);
template CAFFE2_API qint32 requantize_val<qint32, qint32>(double, int64_t, double, int64_t, qint32);

} // namespace at
