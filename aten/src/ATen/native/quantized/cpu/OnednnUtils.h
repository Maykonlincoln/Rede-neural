#pragma once

#include <ATen/Config.h>
#if AT_MKLDNN_ENABLED()
#include <ATen/Tensor.h>
#include <ATen/native/quantized/PackedParams.h>
#include <ideep.hpp>
#include <memory>
#include <mutex>

using PrimitiveCacheKey = std::tuple<
    double, // input_scale
    int64_t, // input_zero_point
    std::vector<int64_t>, // input_shape
    double, // output_scale
    int64_t, // output_zero_point
    int64_t>; // OMP_number_of_threads

enum CacheKeyIndex {
  InputScale,
  InputZeroPoint,
  InputShape,
  OutputScale,
  OutputZeroPoint,
  NumOfThreads,
};

// Base class of primitive cache
struct PrimitiveCache {
  PrimitiveCacheKey key;

  bool hit(const PrimitiveCacheKey& key) {
    return this->key == key;
  }
};

using LinearParams = ideep::matmul_forward_params;
using Conv = dnnl::convolution_forward;
using ConvDesc = dnnl::convolution_forward::primitive_desc;
using ConvParams = ideep::convolution_forward_params;
using Deconv = dnnl::deconvolution_forward;
using DeconvDesc = dnnl::deconvolution_forward::primitive_desc;
using DeconvParams = ideep::deconv_forward_params;

struct LinearPrimitiveCache : PrimitiveCache {
  LinearPrimitiveCache() {}

  LinearPrimitiveCache(
      const PrimitiveCacheKey& key,
      const LinearParams& param) {
    this->key = key;
    this->param = param;
  }

  LinearPrimitiveCache(
      const PrimitiveCacheKey& key,
      const LinearParams& param,
      const ideep::tensor& bias) {
    this->key = key;
    this->param = param;
    if (!bias.is_empty()) {
      expected_bias =
          bias.reorder_if_differ_in(param.pd.bias_desc(), param.bias_attr);
    }
  }

  LinearParams param;
  ideep::tensor expected_bias;

  // For dynamic qlinear, scale and zero point
  // are set at execution time. So we only need to compare
  // the rest part of key.
  bool hit_dynamic(const PrimitiveCacheKey& new_key) {
    auto cached_input_shape = std::get<InputShape>(this->key);
    auto new_input_shape = std::get<InputShape>(new_key);
    return (
        cached_input_shape == new_input_shape &&
        std::get<NumOfThreads>(this->key) == std::get<NumOfThreads>(new_key));
  }

  LinearParams& get_param() {
    return param;
  }

  ideep::tensor& get_expected_bias() {
    return expected_bias;
  }
};

struct ConvPrimitiveCache : PrimitiveCache {
  ConvPrimitiveCache() {}

  ConvPrimitiveCache(
      const PrimitiveCacheKey& key,
      const ConvParams& params,
      const ideep::tensor& bias) {
    this->key = key;
    this->params = params;
    if (!bias.is_empty()) {
      this->expected_bias =
          bias.reorder_if_differ_in(params.pd.bias_desc(), params.bias_attr);
    }
  }

  ideep::tensor expected_bias;
  ConvParams params;

  ConvParams& get_params() {
    return params;
  }

  ideep::tensor& get_bias() {
    return expected_bias;
  }
};

struct DeconvPrimitiveCache : PrimitiveCache {
  DeconvPrimitiveCache() {}

  DeconvPrimitiveCache(
      const PrimitiveCacheKey& key,
      const DeconvParams& params,
      const ideep::tensor& bias) {
    this->key = key;
    this->params = params;
    if (!bias.is_empty()) {
      this->expected_bias =
          bias.reorder_if_differ_in(params.pd.bias_desc(), params.bias_attr);
    }
  }

  DeconvParams params;
  ideep::tensor expected_bias;

  DeconvParams& get_params() {
    return params;
  }

  ideep::tensor& get_bias() {
    return expected_bias;
  }
};

struct PackedLinearWeightsOnednn : public LinearPackedParamsBase {
  PackedLinearWeightsOnednn(
      std::unique_ptr<ideep::tensor> weight,
      c10::optional<ideep::tensor> bias,
      at::Tensor orig_weight,
      c10::optional<at::Tensor> orig_bias)
      : weight_(std::move(weight)),
        bias_(std::move(bias)),
        orig_weight_(std::move(orig_weight)),
        orig_bias_(std::move(orig_bias)) {
    cache_initialized_flag = std::make_unique<std::once_flag>();
  }
  std::unique_ptr<ideep::tensor> weight_;
  c10::optional<ideep::tensor> bias_;
  at::Tensor orig_weight_;
  c10::optional<at::Tensor> orig_bias_;

  at::Tensor apply(
      at::Tensor input,
      double output_scale,
      int64_t output_zero_point) override;
  at::Tensor apply_relu(
      at::Tensor input,
      double output_scale,
      int64_t output_zero_point) override;

  at::Tensor apply_dynamic(at::Tensor input, bool reduce_range=false) override;
  at::Tensor apply_dynamic_relu(at::Tensor input, bool reduce_range=false) override;

  std::tuple<at::Tensor, c10::optional<at::Tensor>> unpack() override;

  c10::optional<at::Tensor> bias() override {
    return orig_bias_;
  }

  static c10::intrusive_ptr<LinearPackedParamsBase> prepack(
      at::Tensor weight,
      c10::optional<at::Tensor> bias);

 private:
  LinearPrimitiveCache prim_cache;
  std::unique_ptr<std::once_flag> cache_initialized_flag;

  template <bool ReluFused>
  at::Tensor apply_impl(
      at::Tensor input,
      double output_scale,
      int64_t output_zero_point);

  template <bool ReluFused>
  at::Tensor apply_dynamic_impl(at::Tensor input, bool reduce_range=false);

  LinearPrimitiveCache& get_cache() {
    return prim_cache;
  }
};

template <int kSpatialDim = 2>
struct PackedConvWeightsOnednn : public ConvPackedParamsBase<kSpatialDim> {
  PackedConvWeightsOnednn(
      std::unique_ptr<ideep::tensor> weight,
      c10::optional<ideep::tensor> bias,
      at::Tensor orig_weight,
      c10::optional<at::Tensor> orig_bias,
      torch::List<int64_t> stride,
      torch::List<int64_t> padding,
      torch::List<int64_t> output_padding,
      torch::List<int64_t> dilation,
      int64_t groups,
      uint8_t transpose)
      : weight_(std::move(weight)),
        bias_(std::move(bias)),
        orig_weight_(std::move(orig_weight)),
        orig_bias_(std::move(orig_bias)),
        stride_(std::move(stride)),
        padding_(std::move(padding)),
        output_padding_(std::move(output_padding)),
        dilation_(std::move(dilation)),
        groups_(groups),
        transpose_(transpose) {
    cache_initialized_flag = std::make_unique<std::once_flag>();
  }

  std::unique_ptr<ideep::tensor> weight_;
  c10::optional<ideep::tensor> bias_;
  at::Tensor orig_weight_;
  c10::optional<at::Tensor> orig_bias_;
  torch::List<int64_t> stride_;
  torch::List<int64_t> padding_;
  torch::List<int64_t> output_padding_;
  torch::List<int64_t> dilation_;
  int64_t groups_;
  uint8_t transpose_;

  at::Tensor apply(
      const at::Tensor& input,
      double output_scale,
      int64_t output_zero_point) override;

  at::Tensor apply_relu(
      const at::Tensor& input,
      double output_scale,
      int64_t output_zero_point) override;

  at::Tensor apply_dynamic(
      const at::Tensor& input,
      bool reduce_range) override;

  std::tuple<at::Tensor, c10::optional<at::Tensor>> unpack() override;

  static c10::intrusive_ptr<ConvPackedParamsBase<kSpatialDim>> prepack(
      at::Tensor weight,
      c10::optional<at::Tensor> bias,
      torch::List<int64_t> stride,
      torch::List<int64_t> padding,
      torch::List<int64_t> output_padding,
      torch::List<int64_t> dilation,
      int64_t groups,
      bool transpose);

  torch::List<int64_t> stride() const override {
    return stride_;
  }

  torch::List<int64_t> padding() const override {
    return padding_;
  }

  torch::List<int64_t> output_padding() const override {
    return output_padding_;
  }

  torch::List<int64_t> dilation() const override {
    return dilation_;
  }

  int64_t groups() const override {
    return groups_;
  }

  bool transpose() const override {
    return (bool)transpose_;
  }

 private:
  ConvPrimitiveCache conv_prim_cache;
  DeconvPrimitiveCache deconv_prim_cache;
  std::unique_ptr<std::once_flag> cache_initialized_flag;

  template <bool ReluFused>
  at::Tensor apply_impl(
      const at::Tensor& input,
      double output_scale,
      int64_t output_zero_point);

  ConvPrimitiveCache& get_conv_cache() {
    assert(!transpose());
    return conv_prim_cache;
  }

  DeconvPrimitiveCache& get_deconv_cache() {
    assert(transpose());
    return deconv_prim_cache;
  }
};

#endif // #if AT_MKLDNN_ENABLED()
