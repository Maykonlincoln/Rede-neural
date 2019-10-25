#pragma once

#include <torch/csrc/jit/ir.h>

namespace torch {
namespace jit {

// propagate autograd zero information through a gradient graph and
// remove grad_of blocks if present.
// Note: this is a very limited pass. It only propagates autograd zeros for
// operations generated by the symbolic autodiff code and cleans up
// AutogradAdds when possible. Outputs of other nodes are conservatively
// marked Unknown and not optimized.
TORCH_API void specializeAutogradZero(Graph &g);

} // namespace jit
} // namespace torch
