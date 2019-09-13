#ifndef THC_GENERIC_FILE
#define THC_GENERIC_FILE "THC/generic/THCTensorRandom.h"
#else

#include "ATen/core/Generator.h"

#if defined(THC_REAL_IS_FLOAT) || defined(THC_REAL_IS_DOUBLE) || defined(THC_REAL_IS_HALF)

THC_API void THCTensor_(multinomial)(struct THCState *state, THCudaLongTensor *self, THCTensor *prob_dist, int n_sample, int with_replacement, at::Generator* gen_);
THC_API void THCTensor_(multinomialAliasSetup)(struct THCState *state, THCTensor *probs, THCudaLongTensor *J, THCTensor *q);
THC_API void THCTensor_(multinomialAliasDraw)(THCState *state, THCudaLongTensor *self, THCTensor *_q, THCudaLongTensor *_J, int n_sample, at::Generator* gen_);

#endif
#endif
