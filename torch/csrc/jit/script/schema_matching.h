#pragma once
#include <torch/csrc/WindowsTorchApiMacro.h>
#include <torch/csrc/jit/ir.h>
#include <torch/csrc/jit/named_value.h>

#include <ATen/core/function_schema.h>

namespace torch {
namespace jit {
namespace script {

// try to match a list if inputs and keyword 'attributes' to this schema,
// if it works return the flat list of positional inputs to the call
// if it returns nullopt, then failure_messages contains a good error report
// set convert_tensor_to_num to true if ImplicitTensorToNums should be inserted
// to match the schema

struct MatchedSchema {
  std::vector<Value*> inputs;
  std::vector<TypePtr> return_types;
  c10::OptNameList return_field_names;
};

TORCH_API MatchedSchema matchSchema(
    const ::c10::FunctionSchema& schema,
    const SourceRange& loc,
    Graph& graph,
    at::ArrayRef<NamedValue> args,
    at::ArrayRef<NamedValue> kwarg,
    const c10::optional<NamedValue>& self = c10::nullopt);

TORCH_API std::pair<size_t, MatchedSchema> matchSchemas(
    const std::vector<const ::c10::FunctionSchema*>& schemas,
    const SourceRange& loc,
    Graph& graph,
    at::ArrayRef<NamedValue> args,
    at::ArrayRef<NamedValue> kwarg,
    const c10::optional<NamedValue>& self = c10::nullopt,
    bool render_errors = false);

TORCH_API bool convertibleToList(
    const TypePtr& type,
    const TypePtr& list_type_);

TORCH_API Value* emitBuiltinCall(
    const SourceRange& loc,
    Graph& graph,
    Symbol name,
    at::ArrayRef<NamedValue> inputs,
    at::ArrayRef<NamedValue> attributes,
    const c10::optional<NamedValue>& self = c10::nullopt);

TORCH_API c10::optional<size_t> findInputWithName(
    const std::string& name,
    at::ArrayRef<NamedValue> kwargs);

// applies implict conversion from value trying to turn it into type
// concrete_type it succeeds if the return_value->isSubtypeOf(concrete_type)
TORCH_API Value* tryConvertToType(
    const SourceRange& loc,
    Graph& graph,
    const TypePtr& concrete_type,
    Value* value,
    bool allow_conversions);
} // namespace script
} // namespace jit
} // namespace torch
