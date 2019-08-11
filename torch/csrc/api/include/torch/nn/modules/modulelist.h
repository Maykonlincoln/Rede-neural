#pragma once

#include <torch/nn/cloneable.h>
#include <torch/nn/module.h>
#include <torch/nn/modules/any.h>

#include <vector>

namespace torch {
namespace nn {

/// A list of `Module`s that registers its elements.
///
/// \rst
/// .. code-block:: cpp
///
///   torch::nn::ModuleList mlist(
///     torch::nn::Linear(3, 4),
///     torch::nn::BatchNorm(4),
///     torch::nn::Dropout(0.5)
///   );
///
///   for (const auto &module : mlist) {
///     module.pretty_print();
///   }
///
/// \endrst
///
/// Why should you use `ModuleList` instead of a simple `std::vector`? The value
/// a `Sequential` provides over manually calling a sequence of modules is that
/// it allows treating the whole container *as a single module*, such that
/// performing a transformation on the `ModuleList` applies to each of the
/// modules it stores (which are each a registered submodule of the
/// `ModuleList`). For example, calling
/// `.to(torch::kCUDA)` on a `Sequential` will move each module in the list to
/// CUDA memory. For example:
///
/// \rst
/// .. code-block:: cpp
///
///   torch::nn::ModuleList mlist(
///     torch::nn::Linear(3, 4),
///     torch::nn::BatchNorm(4),
///     torch::nn::Dropout(0.5)
///   );
///
///   // Convert all modules to CUDA.
///   mlist->to(torch::kCUDA);
///
/// \endrst
///
/// Finally, `ModuleList` provides a lightweight container API, such as allowing
/// iteration over submodules, positional access, adding a new module after
/// construction via `push_back`, as well as joining two `ModuleList`s via
/// `extend`.
class ModuleListImpl : public Cloneable<ModuleListImpl> {
 public:
  using Iterator = std::vector<AnyModule>::iterator;
  using ConstIterator = std::vector<AnyModule>::const_iterator;

  ModuleListImpl() = default;

  /// Constructs the `ModuleList` from a variadic list of modules.
  template <typename... Modules>
  explicit ModuleListImpl(Modules&&... modules) {
    modules_.reserve(sizeof...(Modules));
    push_back(std::forward<Modules>(modules)...);
  }

  /// Special cloning function for `ModuleList` because it does not use
  /// `reset()`.
  std::shared_ptr<Module> clone(
      const optional<Device>& device = nullopt) const override {
    auto clone = std::make_shared<ModuleListImpl>();
    for (const auto& module : modules_) {
      clone->push_back(module.clone(device));
    }
    return std::move(clone);
  }

  /// `reset()` is empty for `ModuleList`, since it does not have parameters of
  /// its own.
  void reset() override {}

  /// Pretty prints the `ModuleList` module into the given `stream`.
  void pretty_print(std::ostream& stream) const override {
    stream << "torch::nn::ModuleList";
  }

  /// Adds a new (boxed) `Module` to the `ModuleList` container.
  template <typename ModuleType>
  void push_back(std::shared_ptr<ModuleType> module_ptr) {
    push_back(
        std::to_string(modules_.size()), AnyModule(std::move(module_ptr)));
  }

  /// Adds a new `Module` to the `ModuleList` container, moving or copying it
  /// into a `shared_ptr` internally. This method allows passing value types,
  /// and letting the container deal with the boxing. This means you can write
  /// `ModuleList(Module(3, 4))` instead of
  /// `ModuleList(std::make_shared<Module>(3, 4))`.
  template <typename M, typename = torch::detail::enable_if_module_t<M>>
  void push_back(M&& module) {
    using Type = typename std::remove_reference<M>::type;
    push_back(
        std::to_string(modules_.size()),
        std::make_shared<Type>(std::forward<M>(module)));
  }

  /// Unwraps the contained module of a `ModuleHolder` and adds it to the
  /// `ModuleList`.
  template <typename M>
  void push_back(const ModuleHolder<M>& module_holder) {
    push_back(std::to_string(modules_.size()), module_holder.ptr());
  }

  /// Iterates over the container and calls `push_back()` on each value.
  template <typename Container>
  void extend(const Container& container) {
    for (const auto& module : container) {
      push_back(module);
    }
  }

  /// Returns an iterator to the start of the `ModuleList`.
  Iterator begin() {
    return modules_.begin();
  }

  /// Returns a const iterator to the start of the `ModuleList`.
  ConstIterator begin() const {
    return modules_.begin();
  }

  /// Returns an iterator to the end of the `ModuleList`.
  Iterator end() {
    return modules_.end();
  }

  /// Returns a const iterator to the end of the `ModuleList`.
  ConstIterator end() const {
    return modules_.end();
  }

  /// Attempts to return the module at the given index as the requested type.
  /// Throws an exception if the index is out of bounds or the types do not
  /// match.
  template <typename T>
  T& at(size_t index) {
    static_assert(
        torch::detail::is_module<T>::value,
        "Can only call ModuleList::at with an nn::Module type");
    TORCH_CHECK(index < size(), "Index out of range");
    return modules_[index].get<T>();
  }

  /// Attempts to return the module at the given index as the requested type.
  /// Throws an exception if the index is out of bounds or the types do not
  /// match.
  template <typename T>
  const T& at(size_t index) const {
    static_assert(
        torch::detail::is_module<T>::value,
        "Can only call ModuleList::at with an nn::Module type");
    TORCH_CHECK(index < size(), "Index out of range");
    return modules_[index].get<T>();
  }

  /// Attempts to return a `std::shared_ptr` whose dynamic type is that of the
  /// underlying module at the given index. Throws an exception if the index is
  /// out of bounds.
  std::shared_ptr<Module> ptr(size_t index) const {
    TORCH_CHECK(index < size(), "Index out of range");
    return modules_[index].ptr();
  }

  /// Attempts to return a `std::shared_ptr` whose type is the one provided.
  /// Throws an exception if the index is out of bounds or the types do not
  /// match.
  template <typename T>
  std::shared_ptr<T> ptr(size_t index) const {
    static_assert(
        torch::detail::is_module<T>::value,
        "Can only call ModuleList::ptr with an nn::Module type");
    TORCH_CHECK(index < size(), "Index out of range");
    return modules_[index].ptr<T>();
  }

  /// Like `ptr(index)`.
  std::shared_ptr<Module> operator[](size_t index) const {
    // This is the only method we can call without a type.
    return ptr(index);
  }

  /// The current size of the `ModuleList` container.
  size_t size() const noexcept {
    return modules_.size();
  }

  /// True if there are no modules in the `ModuleList`.
  bool is_empty() const noexcept {
    return size() == 0;
  }

 private:
  /// Takes a First *and* Second parameter, to avoid ambiguity when a parameter
  /// pack has only one type, in which case the template would be preferred,
  /// even if the other `push_back` functions are better fits (e.g. `unique_ptr`
  /// -> `shared_ptr` overload).
  /// NOTE: We explicitly avoid matching this template with
  /// `push_back(std::string("name"), module)` or `push_back("name", module)`,
  /// since they should be handled by their respective `push_back` functions.
  template <
      typename First,
      typename Second,
      typename... Rest,
      typename = torch::disable_if_t<
          std::is_same<First, std::string>::value ||
          std::is_same<
              typename std::decay<First>::type,
              std::decay<const char (&)[]>::type>::value>>
  void push_back(First&& first, Second&& second, Rest&&... rest) {
    push_back(std::forward<First>(first));
    // Recursively calls this method, until the parameter pack only thas this
    // entry left. Then calls `push_back()` a final time (above).
    push_back(std::forward<Second>(second), std::forward<Rest>(rest)...);
  }

  /// The base case, when the list of modules is empty.
  void push_back() {}

  /// Adds a `Module` to the `ModuleList`.
  void push_back(std::string name, Module module) {
    modules_.push_back(std::move(module));
    const auto index = modules_.size() - 1;
    register_module(std::to_string(index), modules_[index]);
  }

  // Box the AnyModules to give ModuleList reference semantics, like the rest of
  // the API. Note that this is not required otherwise, this could just be a
  // `vector<AnyModule>`.
  std::vector<AnyModule> modules_;
};

/// A `ModuleHolder` subclass for `ModuleListImpl`.
/// See the documentation for `ModuleListImpl` class to learn what methods it
/// provides, or the documentation for `ModuleHolder` to learn about PyTorch's
/// module storage semantics.
TORCH_MODULE(ModuleList);

} // namespace nn
} // namespace torch
