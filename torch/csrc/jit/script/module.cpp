#include <torch/csrc/jit/script/module.h>
#include <c10/util/Exception.h>
#include <torch/csrc/autograd/generated/variable_factories.h>
#include <torch/csrc/jit/export.h>
#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/operator.h>
#include <torch/csrc/jit/passes/dead_code_elimination.h>
#include <torch/csrc/jit/passes/inliner.h>
#include <torch/csrc/jit/script/compiler.h>
#include <torch/csrc/jit/script/error_report.h>
#include <torch/csrc/jit/script/schema_matching.h>

namespace torch {
namespace jit {
namespace script {

EntityType get_entity_type(ClassTypePtr type, size_t offset_) {
  TORCH_CHECK(offset_ < type->numAttributes());
  if (type->is_parameter(offset_)) {
    return EntityType::PARAMETER;
  }
  at::TypePtr t = type->getAttribute(offset_);
  if (auto cls = t->cast<at::ClassType>()) {
    if (cls->is_module()) {
      return EntityType::MODULE;
    }
  }
  return EntityType::ATTRIBUTE;
}

c10::optional<EntityType> get_entity_type(ClassTypePtr type, const std::string& name) {
  if (auto slot_idx = type->findAttributeSlot(name)) {
    return get_entity_type(type, *slot_idx);
  }
  return c10::nullopt;
}

EntityType get_entity_type(ShadowClassTypePtr type, size_t offset_) {
  TORCH_CHECK(offset_ < type->numAttributes());
  if (type->is_parameter(offset_)) {
    return EntityType::PARAMETER;
  }
  auto mt = type->getMutableAttribute(offset_);
  if (c10::holds_alternative<ShadowClassTypePtr>(mt)) {
    auto cls = c10::get<ShadowClassTypePtr>(mt);
    TORCH_CHECK(cls->is_module(), "Only module can have ShadowClassType");
    return EntityType::MODULE;
  }

  auto t = c10::get<TypePtr>(mt);
  if (auto cls = t->cast<c10::ClassType>()) {
    if (cls->is_module()) {
      return EntityType::MODULE;
    }
  }
  return EntityType::ATTRIBUTE;
}

c10::optional<EntityType> get_entity_type(ShadowClassTypePtr type, const std::string& name) {
  if (auto slot_idx = type->findMutableAttributeSlot(name)) {
    return get_entity_type(type, *slot_idx);
  }
  return c10::nullopt;
}

static ModulePtr create_module_object(
    c10::QualifiedName class_name,
    std::shared_ptr<CompilationUnit> cu,
    bool shouldMangle = false) {
  // If the name is unqualified, prepend a `__torch__`, similar to what Python
  // does with `__main__` for top-level code.
  if (class_name.prefix().empty()) {
    class_name = c10::QualifiedName("__torch__", class_name.name());
  }
  if (shouldMangle && cu->get_class(class_name) != nullptr) {
    class_name = cu->mangle(class_name);
  }
  auto cls = ClassType::create(std::move(class_name), cu, /*is_module=*/true);
  cu->register_type(cls);
  return c10::ivalue::Object::create(
      c10::StrongTypePtr(std::move(cu), std::move(cls)), 0);
}

Module::Module(c10::QualifiedName class_name)
    : module_value_(create_module_object(
          std::move(class_name),
          std::make_shared<CompilationUnit>())) {}

Module::Module(
    std::shared_ptr<CompilationUnit> cu,
    const c10::ClassTypePtr& type)
    : module_value_(c10::ivalue::Object::create(
          c10::StrongTypePtr(std::move(cu), type),
          type->numAttributes())) {}

Module::Module(
    c10::QualifiedName class_name,
    std::shared_ptr<CompilationUnit> cu,
    bool shouldMangle)
    : module_value_(create_module_object(
          std::move(class_name),
          std::move(cu),
          shouldMangle)) {}

ModulePtr Module::module_object() const {
  if (!module_value_) {
    // User has created a Model without assigning it to something already
    // loaded. This is done in tests, and when using the .define method.
    module_value_ =
        create_module_object("Module", std::make_shared<CompilationUnit>());
  }
  return module_value_;
}

// first class mode runs models as first class objects,
// and does not force inlining everywhere. This is experimental
// as we bring up the system since it will degrade performance
// and may introduce bugs. test_jit.py provides context managers
// that enable it for specific tests.
thread_local bool inline_everything = true;
bool& getInlineEverythingMode() {
  return inline_everything;
}

void Module::to(at::Device device, at::ScalarType dtype, bool non_blocking) {
  to_impl(device, dtype, non_blocking);
}

void Module::to(at::ScalarType dtype, bool non_blocking) {
  to_impl(/*device=*/c10::nullopt, dtype, non_blocking);
}

void Module::to(at::Device device, bool non_blocking) {
  to_impl(device, /*dtype=*/c10::nullopt, non_blocking);
}

void Module::save(std::ostream& out, const ExtraFilesMap& extra_files) const {
#ifndef C10_MOBILE
  ExportModule(*this, out, extra_files, false);
#else
  AT_ERROR("Saving module is not supported on mobile.");
#endif
}

void Module::save(const std::string& filename, const ExtraFilesMap& extra_files)
    const {
#ifndef C10_MOBILE
  ExportModule(*this, filename, extra_files, false);
#else
  AT_ERROR("Saving module is not supported on mobile.");
#endif
}

void Module::_save_for_mobile(std::ostream& out, const ExtraFilesMap& extra_files) const {
#ifndef C10_MOBILE
  ExportModule(*this, out, extra_files, true);
#else
  AT_ERROR("Saving module is not supported on mobile.");
#endif
}

void Module::_save_for_mobile(const std::string& filename, const ExtraFilesMap& extra_files)
    const {
#ifndef C10_MOBILE
  ExportModule(*this, filename, extra_files, true);
#else
  AT_ERROR("Saving module is not supported on mobile.");
#endif
}

void module_state_to(
    const IValue& iv,
    const c10::optional<at::Device>& device,
    const c10::optional<at::ScalarType>& dtype,
    bool non_blocking) {
  // Need to access the `at::Tensor` as a `Variable` here.
  autograd::Variable variable = iv.toTensor();
  // Use the data's original device or dtype if not supplied here.
  auto new_data = variable.to(
      device.value_or(variable.device()),
      dtype.value_or(variable.scalar_type()),
      non_blocking);
  variable.set_data(new_data);
}

void Module::to_impl(
    const c10::optional<at::Device>& device,
    const c10::optional<at::ScalarType>& dtype,
    bool non_blocking) {
  // First call `to()` on every child module.
  for (NameModule m : get_modules()) {
    m.module.to_impl(device, dtype, non_blocking);
  }
  // Then convert every of our parameters.
  for (NameValue parameter : get_parameters()) {
    module_state_to(parameter.value, device, dtype, non_blocking);
  }
  // Then convert every tensor attributes (buffers).
  for (NameValue attr : get_attributes()) {
    if (attr.value.type()->isSubtypeOf(TensorType::get())) {
      module_state_to(attr.value, device, dtype, non_blocking);
    }
  }
}

Method::Method(ModulePtr owner, Function* function)
    : owner_(std::move(owner)), function_(function) {}

Module Method::owner() const {
  return Module(owner_);
}
void Method::run(Stack& stack) {
  stack.insert(stack.begin(), owner().module_object());
  function_->run(stack);
}

IValue Method::operator()(std::vector<IValue> stack, const Kwargs& kwargs) {
  stack.insert(stack.begin(), owner().module_object());
  return (*function_)(std::move(stack), kwargs);
}

void Module::define(const std::string& src, const ResolverPtr& resolver) {
  const auto self = SimpleSelf(type());
  class_compilation_unit()->define(
      name(), src, resolver ? resolver : script::nativeResolver(), &self);
}

void Module::clone_method(
    const Module& orig,
    const Function& method,
    const std::unordered_map<TypePtr, TypePtr>& type_remap) {
  // type remapping - when we copy method implementations from one module
  // singleton to another, we need to update the types of the self arguments
  // to match the new module.
  // XXX - this only handles modules that occur as variables, not modules
  // that appear in aggregate types. Currently this works fine because
  // we restrict how modules can be used during the lowering step. Eventually,
  // we will need to decide what it means for us to 'copy' a module.
  // For instance, we can copy just the state (parameters, attributes),
  // but share the code. Or we can copy the code. If we choose to copy the
  // code, what should we do about aggregate types that contain a module?
  auto type_remap_fn = [&](TypePtr in) {
    auto it = type_remap.find(in);
    if (it == type_remap.end())
      return in;
    return it->second;
  };
  auto graph = method.graph()->copy();
  graph->remapTypes(type_remap_fn);
  auto schema = method.getSchema().cloneWithRemappedTypes(type_remap_fn);
  const auto this_method_name = getNameForMethod(method.name());
  auto copied =
      class_compilation_unit()->create_function(this_method_name, graph);
  type()->addMethod(copied);
  copied->setSchema(std::move(schema));
}

void Module::clone_method(const Module& orig, const std::string& name) {
  std::unordered_map<TypePtr, TypePtr> type_remap;
  std::vector<std::pair<Module, Module>> to_scan = {{orig, *this}};
  while (!to_scan.empty()) {
    auto entry = to_scan.back();
    to_scan.pop_back();
    type_remap[entry.first.module_object()->type()] =
        entry.second.module_object()->type();
    for (const NameModule& s : entry.first.get_modules()) {
      to_scan.emplace_back(s.module, entry.second.get_module(s.name));
    }
  }
  return clone_method(orig, orig.get_method(name).function(), type_remap);
}

Module Module::clone() const {
  std::unordered_map<TypePtr, TypePtr> type_remap;
  return clone_impl(type_remap);
}

Module Module::clone_impl(
    std::unordered_map<TypePtr, TypePtr>& type_remap) const {
  // Create a new module_object in the same compilation unit.
  // The name is the same as for the original module, but it'll be mangled.
  // The class type is also created from scratch.
  Module r(name(), class_compilation_unit(), true);
  type_remap[type()] = r.type();

  // Copy slots. If a slot is a module - recursively clone it.
  for (const NameValue& s : get_slots()) {
    if (*entity_type(s.name) == EntityType::MODULE) {
      const Module& orig = Module(s.value.toObject());
      Module cloned = orig.clone_impl(type_remap);
      type_remap[orig.type()] = cloned.type();
      r.register_module(s.name, cloned);
    } else {
      r.register_attribute(
          s.name,
          s.value.type(),
          s.value,
          *entity_type(s.name) == EntityType::PARAMETER);
    }
  }

  // Clone methods remapping the types to the cloned ones.
  for (auto& fn : type()->methods()) {
    r.clone_method(*this, *fn, type_remap);
  }
  return r;
}

Module Module::create_module_from_shadow_impl(
    const Module& m,
    const ShadowClassTypePtr& s_cls,
    std::unordered_map<TypePtr, TypePtr>& type_remap) {
  Module r(m.name(), m.class_compilation_unit(), true);
  type_remap[m.type()] = r.type();

  // Copy slots. If a slot is a module - recursively clone it.
  for (const auto& name: s_cls->attributeNames()) {
    auto slot_idx = m.type()->findAttributeSlot(name);
    // TODO: type check for m.type() and s_cls
    const auto& value = slot_idx ? m.module_object()->getSlot(*slot_idx) : IValue();
    auto mutable_type = s_cls->getMutableAttribute(name).value();
    if (get_entity_type(s_cls, name) == EntityType::MODULE) {
      const Module& orig = Module(value.toObject());
      Module cloned;
      if (c10::holds_alternative<ShadowClassTypePtr>(mutable_type)) {
        cloned = create_module_from_shadow_impl(orig, c10::get<ShadowClassTypePtr>(mutable_type), type_remap);
      } else {
        cloned = orig.clone_impl(type_remap);
      }
      type_remap[orig.type()] = cloned.type();
      r.register_module(name, cloned);
    } else {
      TORCH_CHECK(c10::holds_alternative<TypePtr>(mutable_type),
                  "Expected mutable module attribute to have type of Type");
      r.register_attribute(
          name,
          c10::get<TypePtr>(mutable_type),
          value,
          get_entity_type(s_cls, name) == EntityType::PARAMETER);
    }
  }

  // Clone methods remapping the types to the cloned ones.
  for (auto& fn : m.type()->methods()) {
    r.clone_method(m, *fn, type_remap);
  }
  return r;
}

void Module::train(bool on) {
  for (NameModule s : get_modules()) {
    s.module.train(on);
  }
  if (auto slot = find_attribute("training")) {
    set_attribute("training", on);
  } else {
    TORCH_INTERNAL_ASSERT("'training' attribute not found");
  }
}

IValue Module::create_class(const c10::QualifiedName& name, Stack stack) const {
  // Look up the class
  const auto classType =
      class_compilation_unit()->get_class(c10::QualifiedName(name));
  if (!classType) {
    AT_ERROR(
        "Could not find class with name: '",
        name.qualifiedName(),
        "' in module.");
  }

  // Create a bare object with correct number of slots
  const size_t numAttrs = classType->numAttributes();
  auto obj = c10::ivalue::Object::create(
      c10::StrongTypePtr(class_compilation_unit(), classType), numAttrs);

  // Invoke the `__init__()` of the class with the arguments provided.
  Stack stackWithSelf = {obj};
  for (auto& arg : stack) {
    stackWithSelf.push_back(std::move(arg));
  }
  // Note: following Python, `__init__()` modifies its first parameter in-place
  // and returns nothing.
  classType->getMethod("__init__")->operator()(std::move(stackWithSelf));

  return obj;
}

ivalue_list Module::get_parameters() const {
  return ivalue_list(*this, EntityType::PARAMETER);
}

ivalue_list Module::get_attributes() const {
  return ivalue_list(*this, EntityType::ATTRIBUTE);
}

ivalue_list Module::get_slots() const {
  return ivalue_list(*this, c10::nullopt);
}

module_list Module::get_modules() const {
  return module_list(*this, EntityType::MODULE);
}

c10::optional<autograd::Variable> Module::find_parameter(
    const std::string& name) const {
  auto slot_idx = type()->findAttributeSlot(name);
  if (slot_idx && type()->is_parameter(*slot_idx)) {
    return autograd::as_variable_ref(
        module_object()->getSlot(*slot_idx).toTensor());
  }
  return c10::nullopt;
}

c10::optional<IValue> Module::find_attribute(const std::string& name) const {
  auto slot_idx = type()->findAttributeSlot(name);
  if (slot_idx && !type()->is_parameter(*slot_idx) &&
      !type()->is_module(*slot_idx)) {
    return module_object()->getSlot(*slot_idx);
  }
  return c10::nullopt;
}

c10::optional<autograd::Variable> Module::find_buffer(
    const std::string& name) const {
  auto slot_idx = type()->findAttributeSlot(name);
  if (slot_idx && !type()->is_parameter(*slot_idx) &&
      !type()->is_module(*slot_idx) &&
      type()->getAttribute(*slot_idx)->isSubtypeOf(TensorType::get())) {
    return autograd::as_variable_ref(
        module_object()->getSlot(*slot_idx).toTensor());
  }
  return c10::nullopt;
}

c10::optional<Module> Module::find_module(const std::string& name) const {
  auto slot_idx = type()->findAttributeSlot(name);
  if (slot_idx && type()->is_module(*slot_idx)) {
    return Module(module_object()->getAttr(name).toObject());
  }
  return c10::nullopt;
}

c10::optional<Method> Module::find_method(const std::string& basename) const {
  for (Function* fn : type()->methods()) {
    if (fn->name() == basename) {
      return Method(module_object(), fn);
    }
  }
  return c10::nullopt;
}

void Module::apply(const std::function<void(Module&)>& fn) {
  for (NameModule s : get_modules()) {
    s.module.apply(fn);
  }
  fn(*this);
}

std::string Module::dump_to_str(
    bool print_method_bodies,
    bool print_attr_values,
    bool print_param_values,
    int level = 0) const {
  std::stringstream ss;
  std::stringstream parameters_ss;
  std::stringstream attributes_ss;
  std::stringstream methods_ss;
  std::stringstream submodules_ss;

  for (const NameValue& p : get_parameters()) {
    parameters_ss << p.name << " = ";
    if (print_param_values) {
      parameters_ss << p.value.toTensor() << std::endl;
    } else {
      parameters_ss << "..." << std::endl;
    }
  }

  for (const NameValue& p : get_attributes()) {
    attributes_ss << p.name << " = ";
    if (!p.value.isTensor() || print_attr_values) {
      attributes_ss << p.value << std::endl;
    } else {
      attributes_ss << "..." << std::endl;
    }
  }

  for (const Method& method : get_methods()) {
    methods_ss << "  method " << method.name() << " {" << std::endl;
    if (print_method_bodies) {
      methods_ss << torch::jit::jit_log_prefix(
                        "    ", method.graph()->toString())
                 << std::endl;
    }
    methods_ss << "  }" << std::endl;
  }

  ss << "module " << name().qualifiedName() << " {" << std::endl;
  ss << "  parameters {" << std::endl;
  ss << torch::jit::jit_log_prefix("    ", parameters_ss.str());
  ss << "  }" << std::endl;
  ss << "  attributes {" << std::endl;
  ss << torch::jit::jit_log_prefix("    ", attributes_ss.str());
  ss << "  }" << std::endl;
  ss << "  methods {" << std::endl;
  ss << torch::jit::jit_log_prefix("  ", methods_ss.str());
  ss << "  }" << std::endl;
  ss << "  submodules {" << std::endl;
  for (const NameModule& s : get_modules()) {
    // We do level + 2, because one level of indentation comes from 'submodules'
    // scope and the other one goes from a specific submodule we're printing.
    ss << s.module.dump_to_str(
        print_method_bodies, print_attr_values, print_param_values, level + 2);
  }
  ss << "  }" << std::endl;
  ss << "}" << std::endl;

  std::string indent(2 * level, ' ');
  return torch::jit::jit_log_prefix(indent, ss.str());
}

void Module::dump(
    bool print_method_bodies = true,
    bool print_attr_values = true,
    bool print_param_values = true) const {
  std::cout << dump_to_str(
                   print_method_bodies,
                   print_attr_values,
                   print_param_values)
            << std::endl;
}

} // namespace script
} // namespace jit
} // namespace torch
