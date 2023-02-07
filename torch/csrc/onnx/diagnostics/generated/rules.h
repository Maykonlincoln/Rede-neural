#pragma once

/**
 * GENERATED CODE - DO NOT EDIT DIRECTLY
 * This file is generated by gen_diagnostics.py.
 * See tools/onnx/gen_diagnostics.py for more information.
 *
 * Diagnostic rules for PyTorch ONNX export.
 */

namespace torch {
namespace onnx {
namespace diagnostics {

enum class Rule : uint32_t {
  /**
   * @brief Node is missing ONNX shape inference.
   */
  kNodeMissingOnnxShapeInference,

  /**
   * @brief Missing symbolic function for custom PyTorch operator, cannot
   * translate node to ONNX.
   */
  kMissingCustomSymbolicFunction,

  /**
   * @brief Missing symbolic function for standard PyTorch operator, cannot
   * translate node to ONNX.
   */
  kMissingStandardSymbolicFunction,

  /**
   * @brief Operator is supported in newer opset version.
   */
  kOperatorSupportedInNewerOpsetVersion,

  /**
   * @brief FX Tracer succeeded.
   */
  kFxTracerSuccess,

  /**
   * @brief FX Tracer failed.
   */
  kFxTracerFailure,

  /**
   * @brief FX Tracer succeeded.
   */
  kFxFrontendAotautograd,

  /**
   * @brief FX pass converting torch.neg to torch.sigmoid.
   */
  kFxPassConvertNegToSigmoid,

  /**
   * @brief Op level tracking. ToDo, experimenting diagnostics, placeholder
   * text.
   */
  kAtenlibSymbolicFunction,

  /**
   * @brief Graph level tracking. Each op is a step. ToDo, experimenting
   * diagnostics, placeholder text.
   */
  kAtenlibFxToOnnx,

  /**
   * @brief Node level tracking. ToDo, experimenting diagnostics, placeholder
   * text.
   */
  kFxNodeToOnnx,

  /**
   * @brief The make_fx + decomposition pass on fx graph produced from Dynamo,
   * before ONNX export.
   */
  kFxFrontendDynamoMakeFx,

  /**
   * @brief The formatted str for argument to display is too verbose.
   */
  kArgFormatTooVerbose,
};

static constexpr const char* const kPyRuleNames[] = {
    "node_missing_onnx_shape_inference",
    "missing_custom_symbolic_function",
    "missing_standard_symbolic_function",
    "operator_supported_in_newer_opset_version",
    "fx_tracer_success",
    "fx_tracer_failure",
    "fx_frontend_aotautograd",
    "fx_pass_convert_neg_to_sigmoid",
    "atenlib_symbolic_function",
    "atenlib_fx_to_onnx",
    "fx_node_to_onnx",
    "fx_frontend_dynamo_make_fx",
    "arg_format_too_verbose",
};

} // namespace diagnostics
} // namespace onnx
} // namespace torch
