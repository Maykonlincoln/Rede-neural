#include "THZVector.h"

#include "generic/simd/simd.h"

/* we don't have support for the following */
// #ifdef __NEON__
// #include "vector/NEON.c"
// #endif

// #ifdef __PPC64__
// #include "vector/VSX.c"
// #endif

// #if defined(USE_SSE2) || defined(USE_SSE3) || defined(USE_SSSE3) \
//         || defined(USE_SSE4_1) || defined(USE_SSE4_2)
// #include "vector/SSE.c"
// #endif

#if defined(USE_AVX)
#include "vector/AVX.h"
#endif

#if defined(USE_AVX2)
#include "vector/AVX2.h"
#endif

#include "generic/THZVectorDefault.c"
#include "THZGenerateAllTypes.h"

#include "generic/THZVectorDispatch.c"
#include "THZGenerateAllTypes.h"
