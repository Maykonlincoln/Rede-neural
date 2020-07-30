#include <ATen/core/boxing/impl/boxing.h>
#include <ATen/core/boxing/impl/make_boxed_from_unboxed_functor.h>
#include <ATen/core/boxing/impl/WrapFunctionIntoFunctor.h>
#include <ATen/core/boxing/impl/WrapFunctionIntoRuntimeFunctor.h>

namespace c10 {

inline KernelFunction::KernelFunction()
: functor_(nullptr)
, boxed_kernel_func_(nullptr)
, unboxed_kernel_func_(nullptr)
{}

inline KernelFunction::KernelFunction(std::unique_ptr<OperatorKernel> functor, InternalBoxedKernelFunction* boxed_kernel_func, void* unboxed_kernel_func)
: functor_(std::move(functor))
, boxed_kernel_func_(boxed_kernel_func)
, unboxed_kernel_func_(unboxed_kernel_func)
{}

template<KernelFunction::BoxedKernelFunction* func>
inline void KernelFunction::make_boxed_function(OperatorKernel*, const OperatorHandle& opHandle, Stack* stack) {
    func(opHandle, stack);
}

inline bool KernelFunction::isValid() const {
    // TODO We want to introduce the invariant that all kernels must be callable in a boxed way, then this should only check boxed_kernel_func_.
    return boxed_kernel_func_ != nullptr || unboxed_kernel_func_ != nullptr;
}

inline bool KernelFunction::isFallthrough() const {
    return boxed_kernel_func_ == &fallthrough_kernel;
}

inline void KernelFunction::callBoxed(const OperatorHandle& opHandle, Stack* stack) const {
    checkBoxedKernel(opHandle);
    (*boxed_kernel_func_)(functor_.get(), opHandle, stack);
}

template<class Return, class... Args>
inline Return callUnboxedKernelFunction(void* unboxed_kernel_func, OperatorKernel* functor, Args&&... args) {
    using ActualSignature = Return (OperatorKernel*, Args...);
    ActualSignature* func = reinterpret_cast<ActualSignature*>(unboxed_kernel_func);
    return (*func)(functor, std::forward<Args>(args)...);
}

template<class Return, class... Args>
inline Return KernelFunction::call(const OperatorHandle& opHandle, Args... args) const {
    // note: Args above is intentionally not Args&&. We don't want perfect
    // forwarding, which would require Args to be deduced, but instead we
    // want callers to explicitly specify the Args.

    if (C10_LIKELY(unboxed_kernel_func_ != nullptr)) {
        return callUnboxedKernelFunction<Return, Args...>(unboxed_kernel_func_, functor_.get(), std::forward<Args>(args)...);
    }

    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
        boxed_kernel_func_ != nullptr,
        "Tried to call KernelFunction::call() on an uninitialized KernelFunction."
    );

    return impl::BoxedKernelWrapper<Return(Args...)>::call(
        boxed_kernel_func_,
        functor_.get(),
        opHandle,
        std::forward<Args>(args)...
    );
}

template<KernelFunction::BoxedKernelFunction* func>
inline KernelFunction KernelFunction::makeFromBoxedFunction() {
    return KernelFunction(
        nullptr,  // no functor_ object
        &make_boxed_function<func>,
        nullptr  // no unboxed function pointer
    );
}

inline KernelFunction KernelFunction::makeFallthrough() {
    return KernelFunction(
        nullptr,  // no functor_ object
        &fallthrough_kernel,
        nullptr  // no unboxed function pointer
    );
}

inline KernelFunction KernelFunction::makeNamedNotSupported() {
    return KernelFunction(
        nullptr,  // no functor_ object
        &named_not_supported_kernel,
        nullptr  // no unboxed function pointer
    );
}

template<bool AllowLegacyTypes, class KernelFunctor>
inline KernelFunction KernelFunction::makeFromUnboxedFunctor(std::unique_ptr<OperatorKernel> kernelFunctor) {
    static_assert(guts::is_functor<KernelFunctor>::value, "Tried to call KernelFunction::makeFromUnboxedFunctor<KernelFunctor> but the argument is not a functor.");
    static_assert(std::is_base_of<OperatorKernel, KernelFunctor>::value, "Tried to call KernelFunction::makeFromUnboxedFunctor<KernelFunctor>, but the functor doesn't inherit from c10::OperatorKernel. Please have the functor inherit from it.");

    return KernelFunction(
        std::move(kernelFunctor),
        &impl::make_boxed_from_unboxed_functor<KernelFunctor, AllowLegacyTypes>::call,
        reinterpret_cast<void*>(&impl::wrap_kernel_functor_unboxed<KernelFunctor>::call)
    );
}

template<class KernelFunctor>
inline KernelFunction KernelFunction::makeFromUnboxedOnlyFunctor(std::unique_ptr<OperatorKernel> kernelFunctor) {
    // TODO We want to get rid of kernels that have only an unboxed function pointer.
    //      All kernels should have a boxed pointer.

    static_assert(guts::is_functor<KernelFunctor>::value, "Tried to call KernelFunction::makeFromUnboxedFunctor<KernelFunctor> but the argument is not a functor.");
    static_assert(std::is_base_of<OperatorKernel, KernelFunctor>::value, "Tried to call KernelFunction::makeFromUnboxedFunctor<KernelFunctor>, but the functor doesn't inherit from c10::OperatorKernel. Please have the functor inherit from it.");

    return KernelFunction(
        std::move(kernelFunctor),
        nullptr, // Don't create a boxed kernel for this
        reinterpret_cast<void*>(&impl::wrap_kernel_functor_unboxed<KernelFunctor>::call)
    );
}

template<class FuncPtr, bool AllowLegacyTypes>
inline KernelFunction KernelFunction::makeFromUnboxedFunction(FuncPtr) {
    static_assert(is_compile_time_function_pointer<FuncPtr>::value, "Tried to call KernelFunction::makeFromUnboxedFunction with an invalid parameter. It must be a function pointer created with TORCH_FN.");
    static_assert(!std::is_same<typename FuncPtr::FuncType, BoxedKernelFunction>::value, "Tried to call KernelFunction::makeFromUnboxedFunction with a boxed function pointer. Please use KernelFunction::makeFromBoxedFunction instead.");
    static_assert(FuncPtr::func_ptr() != nullptr, "Kernel function cannot be nullptr");

    return makeFromUnboxedFunctor<AllowLegacyTypes, typename impl::WrapFunctionIntoFunctor<FuncPtr>::type>(
        guts::make_unique_base<OperatorKernel, typename impl::WrapFunctionIntoFunctor<FuncPtr>::type>()
    );
}

template<class FuncPtr>
inline KernelFunction KernelFunction::makeFromUnboxedOnlyFunction(FuncPtr) {
    // TODO We want to get rid of kernels that have only an unboxed function pointer.
    //      All kernels should have a boxed pointer.
    static_assert(is_compile_time_function_pointer<FuncPtr>::value, "Tried to call KernelFunction::makeFromUnboxedOnlyFunction with an invalid parameter. It must be a function pointer created with TORCH_FN.");
    static_assert(!std::is_same<typename FuncPtr::FuncType, BoxedKernelFunction>::value, "Tried to call KernelFunction::makeFromUnboxedOnlyFunction with a boxed function pointer. Please use KernelFunction::makeFromBoxedFunction instead.");
    static_assert(FuncPtr::func_ptr() != nullptr, "Kernel function cannot be nullptr");

    return makeFromUnboxedOnlyFunctor<typename impl::WrapFunctionIntoFunctor<FuncPtr>::type> (
        guts::make_unique_base<OperatorKernel, typename impl::WrapFunctionIntoFunctor<FuncPtr>::type>()
    );
}

template<bool AllowLegacyTypes, class FuncType>
inline KernelFunction KernelFunction::makeFromUnboxedRuntimeFunction(FuncType* func) {
    static_assert(guts::is_function_type<FuncType>::value, "Tried to call KernelFunction::makeFromUnboxedRuntimeFunction with a non-function type.");
    static_assert(!std::is_same<FuncType, BoxedKernelFunction>::value, "Tried to call KernelFunction::makeFromUnboxedRuntimeFunction with a boxed function pointer. Please use KernelFunction::makeFromBoxedFunction instead.");
    TORCH_INTERNAL_ASSERT(func != nullptr, "Kernel function cannot be nullptr");

    return makeFromUnboxedFunctor<AllowLegacyTypes, impl::WrapFunctionIntoRuntimeFunctor<std::decay_t<FuncType>>>(
        guts::make_unique_base<OperatorKernel, impl::WrapFunctionIntoRuntimeFunctor<std::decay_t<FuncType>>>(func)
    );
}

template<class FuncType>
inline KernelFunction KernelFunction::makeFromUnboxedOnlyRuntimeFunction(FuncType* func) {
    static_assert(guts::is_function_type<FuncType>::value, "Tried to call KernelFunction::makeFromUnboxedRuntimeFunction with a non-function type.");
    static_assert(!std::is_same<FuncType, BoxedKernelFunction>::value, "Tried to call KernelFunction::makeFromUnboxedRuntimeFunction with a boxed function pointer. Please use KernelFunction::makeFromBoxedFunction instead.");
    TORCH_INTERNAL_ASSERT(func != nullptr, "Kernel function cannot be nullptr");

    return makeFromUnboxedOnlyFunctor<impl::WrapFunctionIntoRuntimeFunctor<std::decay_t<FuncType>>>(
        guts::make_unique_base<OperatorKernel, impl::WrapFunctionIntoRuntimeFunctor<std::decay_t<FuncType>>>(func)
    );
}

template<bool AllowLegacyTypes, class Lambda>
inline KernelFunction KernelFunction::makeFromUnboxedLambda(Lambda&& lambda) {
    static_assert(guts::is_functor<std::decay_t<Lambda>>::value, "Tried to call KernelFunction::makeFromUnboxedLambda with a non-lambda type.");

    return makeFromUnboxedFunctor<AllowLegacyTypes, impl::WrapFunctionIntoRuntimeFunctor<std::decay_t<Lambda>>>(
        guts::make_unique_base<OperatorKernel, impl::WrapFunctionIntoRuntimeFunctor<std::decay_t<Lambda>>>(std::forward<Lambda>(lambda))
    );
}

inline void KernelFunction::setManuallyBoxedKernel_(InternalBoxedKernelFunction* func) {
    if (boxed_kernel_func_ == &fallthrough_kernel) {
      // special case no-op
      return;
    }
    TORCH_INTERNAL_ASSERT(boxed_kernel_func_ == nullptr, "Tried to set a manually boxed kernel for a kernel that already has a boxed kernel set.");
    TORCH_INTERNAL_ASSERT(unboxed_kernel_func_ != nullptr, "Tried to set a manually boxed kernel for an invalid KernelFunction.");
    boxed_kernel_func_ = func;
}

}
