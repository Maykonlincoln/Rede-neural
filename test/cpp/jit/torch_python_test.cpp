#include <test/cpp/jit/tests.h>

namespace torch {
namespace jit {

#define JIT_TEST(name) test##name();
TORCH_API void runJITCPPTests(bool runCuda) {
  TH_FORALL_TESTS(JIT_TEST)
  if (runCuda) {
    TH_FORALL_TESTS_CUDA(JIT_TEST)
  }

  // This test is special since it requires prior setup in python.
  // So it is not part of the general test list (which is shared between the gtest
  // and python test runners), but is instead invoked manually by the
  // torch_python_test.cpp
  testEvalModeForLoadedModule();
}
#undef JIT_TEST
} // namespace jit
} // namespace torch
