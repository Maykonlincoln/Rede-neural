#if (defined(CUDA_VERSION) && CUDA_VERSION >= 12000) && (!defined(__CUDA_ARCH__) || (__CUDA_ARCH__ >= 800))
#include <cuda_bf16.h>
#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <mma.h>
#endif
#include <ATen/ATen.h>
#include <ATen/core/Tensor.h>
#include <ATen/cuda/CUDAContext.h>
#include <ATen/DeviceGuard.h>
#include <c10/cuda/CUDAGuard.h>


namespace at::native {

template <typename U, typename V>
constexpr __host__ __device__ auto divDown(U a, V b) -> decltype(a + b) {
  static_assert(std::is_integral<U>::value && std::is_integral<V>::value, "");
  return (a / b);
}

template <typename U, typename V>
constexpr __host__ __device__ auto divUp(U a, V b) -> decltype(a + b) {
  static_assert(std::is_integral<U>::value && std::is_integral<V>::value, "");
  // Overflow safe variant of (a + b - 1) / b
  const uint64_t blocks = a / b + (a % b != 0);
  return blocks;
}

template <typename U, typename V>
constexpr __host__ __device__ auto roundDown(U a, V b) -> decltype(a + b) {
  static_assert(std::is_integral<U>::value && std::is_integral<V>::value, "");
  return divDown(a, b) * b;
}

template <typename U, typename V>
constexpr __host__ __device__ auto roundUp(U a, V b) -> decltype(a + b) {
  static_assert(std::is_integral<U>::value && std::is_integral<V>::value, "");
  return divUp(a, b) * b;
}

template <typename U, typename V>
constexpr __host__ __device__ bool isEvenDivisor(U a, V b) {
  static_assert(std::is_integral<U>::value && std::is_integral<V>::value, "");
  return (a % V(b) == 0) && ((a / V(b)) >= 1);
}

template <class T>
constexpr __host__ __device__ T pow(T n, int power) {
  return (power > 0 ? n * pow(n, power - 1) : 1);
}

template <class T>
constexpr __host__ __device__ T pow2(int power) {
  return pow(2, power);
}

static_assert(pow2<int>(8) == 256, "pow2");

template <typename T>
constexpr __host__ __device__ int log2(T n, int p = 0) {
  return (n <= 1) ? p : log2(n / 2, p + 1);
}

static_assert(log2(2) == 1, "log2");
static_assert(log2(3) == 1, "log2");
static_assert(log2(4) == 2, "log2");

template <typename T>
constexpr __host__ __device__ bool isPowerOf2(T v) {
  static_assert(std::is_integral<T>::value, "");
  return (v && !(v & (v - 1)));
}

static_assert(isPowerOf2(2048), "isPowerOf2");
static_assert(!isPowerOf2(3333), "isPowerOf2");

template <typename T>
constexpr __host__ __device__ T nextHighestPowerOf2(T v) {
  static_assert(std::is_integral<T>::value, "");
  return (isPowerOf2(v) ? (T)2 * v : ((T)1 << (log2(v) + 1)));
}

static_assert(nextHighestPowerOf2(1) == 2, "nextHighestPowerOf2");
static_assert(nextHighestPowerOf2(2) == 4, "nextHighestPowerOf2");
static_assert(nextHighestPowerOf2(3) == 4, "nextHighestPowerOf2");
static_assert(nextHighestPowerOf2(4) == 8, "nextHighestPowerOf2");

static_assert(nextHighestPowerOf2(15) == 16, "nextHighestPowerOf2");
static_assert(nextHighestPowerOf2(16) == 32, "nextHighestPowerOf2");
static_assert(nextHighestPowerOf2(17) == 32, "nextHighestPowerOf2");

static_assert(
    nextHighestPowerOf2(1536000000u) == 2147483648u,
    "nextHighestPowerOf2");
static_assert(
    nextHighestPowerOf2((size_t)2147483648ULL) == (size_t)4294967296ULL,
    "nextHighestPowerOf2");

template <typename T>
constexpr __host__ __device__ T nextLowestPowerOf2(T v) {
  static_assert(std::is_integral<T>::value, "");
  return (isPowerOf2(v) ? v / (T)2 : ((T)1 << (log2(v))));
}

static_assert(nextLowestPowerOf2(1) == 0, "nextLowestPowerOf2");
static_assert(nextLowestPowerOf2(2) == 1, "nextLowestPowerOf2");
static_assert(nextLowestPowerOf2(3) == 2, "nextLowestPowerOf2");
static_assert(nextLowestPowerOf2(4) == 2, "nextLowestPowerOf2");

static_assert(nextLowestPowerOf2(15) == 8, "nextLowestPowerOf2");
static_assert(nextLowestPowerOf2(16) == 8, "nextLowestPowerOf2");
static_assert(nextLowestPowerOf2(17) == 16, "nextLowestPowerOf2");

inline __host__ __device__ bool isPointerAligned(const void* p, int align) {
  return reinterpret_cast<uintptr_t>(p) % align == 0;
}

// Returns the increment needed to aligned the pointer to the next highest
// aligned address
template <int Align>
inline __host__ __device__ uint32_t getAlignmentRoundUp(const void* p) {
  static_assert(isPowerOf2(Align), "");
  const uint32_t diff = uint32_t(uintptr_t(p) & uintptr_t(Align - 1));
  return diff == 0 ? 0 : uint32_t(Align) - diff;
}

#if defined(USE_ROCM)
constexpr int32_t kWarpSize = warpSize;
#else
constexpr int32_t kWarpSize = 32;
#endif

#if (defined(CUDA_VERSION) && CUDA_VERSION >= 12000) && (!defined(__CUDA_ARCH__) || (__CUDA_ARCH__ >= 800))
// f16 vector types
struct __align__(2) f16x1 {
  __half vals[1];
};

struct __align__(4) f16x2 {
  __half vals[2];
};

struct __align__(8) f16x4 {
  __half vals[4];
};

struct __align__(16) f16x8 {
  __half vals[8];
};

// bf16 vector types
struct __align__(2) bf16x1 {
  __nv_bfloat16 vals[1];
};

struct __align__(4) bf16x2 {
  __nv_bfloat16 vals[2];
};

struct __align__(8) bf16x4 {
  __nv_bfloat16 vals[4];
};

struct __align__(16) bf16x8 {
  __nv_bfloat16 vals[8];
};

// bf162 vector types
struct __align__(4) bf16x2x1 {
  __nv_bfloat162 vals[1];
};

struct __align__(8) bf16x2x2 {
  __nv_bfloat162 vals[2];
};

struct __align__(16) bf16x2x4 {
  __nv_bfloat162 vals[4];
};

struct __align__(16) bf16x2x4_u32 {
  uint32_t vals[4];
};

struct __align__(8) bf16x2x2_u32 {
  uint32_t vals[2];
};

struct __align__(4) bf16x2x1_u32 {
  uint32_t vals[1];
};

template <typename T, int N>
struct __align__(sizeof(T) * N) VectorType {
  T vals[N];
};

// from
// https://github.com/NVIDIA/FasterTransformer/blob/main/src/fastertransformer/cutlass_extensions/include/cutlass_extensions/interleaved_numeric_conversion.h
inline __device__ bf16x2x4 convert_i4x8_to_bf16x2x4(uint32_t source) {
  bf16x2x4 result;
  constexpr int kElements = 8;

  uint32_t* h = reinterpret_cast<uint32_t*>(&result);
  uint32_t const source_i4s = source;

  // First, we extract the i4s and construct an intermediate fp16 number.
  static constexpr uint32_t immLut = (0xf0 & 0xcc) | 0xaa;
  static constexpr uint32_t MASK = 0x000f000f;
  static constexpr uint32_t I4s_TO_BF16s_MAGIC_NUM = 0x43004300;

  // We don't have enough mantissa to remove as much shift overhead as FP16, so
  // we must loop. No shift needed for first item.
  uint32_t i4s = source_i4s;
  asm volatile("lop3.b32 %0, %1, %2, %3, %4;\n"
               : "=r"(h[0])
               : "r"(i4s), "n"(MASK), "n"(I4s_TO_BF16s_MAGIC_NUM), "n"(immLut));
#pragma unroll
  for (int ii = 1; ii < kElements / 2; ++ii) {
    i4s >>= 4; // or is it 8?
    // (i4s & 0x000f000f) | 0x43004300
    asm volatile(
        "lop3.b32 %0, %1, %2, %3, %4;\n"
        : "=r"(h[ii])
        : "r"(i4s), "n"(MASK), "n"(I4s_TO_BF16s_MAGIC_NUM), "n"(immLut));
  }

  // This is the BF16 {-136, -136} represented as an integer.
  static constexpr uint32_t BF16_BIAS = 0xC308C308;
  static constexpr uint32_t BF16_ONE = 0x3F803F80;

// Finally, we construct the output numbers.
#pragma unroll
  for (int ii = 0; ii < kElements / 2; ++ii) {
    // Since this section is for Ampere+, we use bf16 fma to do the bias
    // subtraction
    asm("fma.rn.bf16x2 %0, %1, %2, %3;\n"
        : "=r"(h[ii])
        : "r"(h[ii]), "r"(BF16_ONE), "r"(BF16_BIAS));
  }

  return result;
}



enum class KReductionType {
  // No k-reduction is needed between blocks as the number of k-tiles processed
  // per block are exact and we can directly write the output
  None,
};

// Loads the A matrix in 16-bit standard m x k row major layout, and writes
// the C matrix in 16-bit standard m x n row major layout:
//
// size [m][k]
template <KReductionType ReduceType>
struct ALayout_RM {
  static constexpr int32_t kMTileSize = 16;
  static constexpr int32_t kNTileSize = 8;
  static constexpr int32_t kKTileSize = 16;

  template <int KTilesToLoad>
  static __device__ void load(
      const void* A,
      int32_t m,
      int32_t k,
      int32_t mTiles,
      int32_t mTile,
      int32_t kTiles,
      int32_t kTileStart,
      int32_t laneId,
      bf16x2x4_u32 out[KTilesToLoad]) {
    const auto mLane = mTile * kMTileSize + (laneId / 4);
    const auto kLane = kTileStart * kKTileSize + (laneId % 4) * 2;

    // access
    // [mTile * kMTileSize + (laneId / 4)]
    // [kTileStart * kKTileSize + (laneId % 4) * 2]
    auto aPtr = reinterpret_cast<const __nv_bfloat16*>(A) + mLane * k + kLane;

    auto aPtrPlus8Rows = aPtr + 8 * k;

    bool m0InBounds = mLane < m;
    bool m1InBounds = (mLane + 8) < m;

#pragma unroll
    for (int i = 0; i < KTilesToLoad; ++i) {
      out[i].vals[0] = m0InBounds
          ? *reinterpret_cast<const uint32_t*>(aPtr + i * kKTileSize)
          : uint32_t(0);
      out[i].vals[1] = m1InBounds
          ? *reinterpret_cast<const uint32_t*>(aPtrPlus8Rows + i * kKTileSize)
          : uint32_t(0);

      out[i].vals[2] = m0InBounds
          ? *reinterpret_cast<const uint32_t*>(aPtr + i * kKTileSize + 8)
          : uint32_t(0);
      out[i].vals[3] = m1InBounds ? *reinterpret_cast<const uint32_t*>(
                                        aPtrPlus8Rows + i * kKTileSize + 8)
                                  : uint32_t(0);
    }
  }

  static __device__ void store(
      void* C,
      int32_t m,
      int32_t n,
      int32_t mOutTiles,
      int32_t mTile,
      int32_t nOutTiles,
      int32_t nTile,
      int32_t laneId,
      const float4& out) {
    static_assert(ReduceType == KReductionType::None, "");

    if constexpr (ReduceType == KReductionType::None) {
      // sum.x / sum.y are written at
      // [laneId / 4], [(laneId % 4) * 2, (laneId % 4) * 2 + 1]
      // sum.z / sum.w are written at
      // [8 + (laneId / 4)], [(laneId % 4) * 2, (laneId % 4) * 2 + 1]
      // i.e., same columns, different row.
      const int outRow = mTile * kMTileSize + (laneId / 4);
      const int outCol = nTile * kNTileSize + (laneId % 4) * 2;

      // Pointer where sum.x / sum.y is written
      auto cPtr = reinterpret_cast<__nv_bfloat16*>(C) + outRow * n + outCol;

      auto v01 = __float22bfloat162_rn(float2{out.x, out.y});
      auto v23 = __float22bfloat162_rn(float2{out.z, out.w});

      if (outRow < m) {
        *reinterpret_cast<__nv_bfloat162*>(cPtr) = v01;
      }

      // sum.z, sum.w at +8 rows from cPtr
      if (outRow + 8 < m) {
        *reinterpret_cast<__nv_bfloat162*>(cPtr + 8 * n) = v23;
      }
    }
  }
};

template <int InnerKTiles, int QGroupSize>
struct BLayout_TC_int4 {
  static constexpr int32_t kInnerKTiles = InnerKTiles;
  static constexpr int32_t kMTileSize = 16;
  static constexpr int32_t kNTileSize = 8;
  static constexpr int32_t kKTileSize = 16;

  template <int KTilesToLoad>
  static __device__ void load(
      // type uint32, size [n / 8][k / (InnerKTiles * 16)][32][InnerKTiles / 2]
      // n / 8: n-tiles (n8)
      // k / (InnerKTiles * 16): TC size per k-tile is 16 (m16n8k16)
      // 32: value per warp lane
      // (InnerKTiles / 2): B layout has 4 values per lane (16 bits) per k-tile.
      // 2 k-tiles packed is a uint32 (hence InnerKTiles == 2 is our smallest
      // value) 4 k-tiles packed is a uint32x2 (64 bits) 8 k-tiles packed is a
      // uint32x4 (128 bits)
      const void* __restrict__ B,
      // size [k / qGroupSize][n][2]
      // Contains the scale and zero point of each of the quantized int4 values
      // within B
      // v_reconstructed = (bf16(B_int4_val) * scale) - zero
      const void* __restrict__ quantizationInfo,
      int32_t n,
      int32_t k,
      int32_t nTiles,
      int32_t nTile,
      int32_t kTiles,
      int32_t kTileStart,
      int32_t laneId,
      bf16x2x4_u32 out[KTilesToLoad / InnerKTiles][InnerKTiles / 2]) {
    // offset [nTile][kTileStart / InnerKTiles][laneId][0]
    auto bPtr = reinterpret_cast<const int32_t*>(B) +
        (((nTile * (kTiles / InnerKTiles) + (kTileStart / InnerKTiles)) *
          kWarpSize) +
         laneId) *
            (InnerKTiles / 2);

    int32_t b_int4[KTilesToLoad / InnerKTiles][InnerKTiles / 2];

#pragma unroll
    for (int i = 0; i < KTilesToLoad / InnerKTiles; ++i) {
      auto bPtrCur = bPtr + i * kWarpSize * (InnerKTiles / 2);

      if constexpr (InnerKTiles == 2) {
        b_int4[i][0] = bPtrCur[0];
      }

      if constexpr (InnerKTiles == 4) {
        // asm volatile("ld.global.cs.v2.u32 {%0, %1}, [%2];\n"
        //              : "=r"(b_int4[i][0]), "=r"(b_int4[i][1])
        //              : "l"(bPtrCur));

        int2 load8 = reinterpret_cast<const int2*>(bPtrCur)[0];
        b_int4[i][0] = load8.x;
        b_int4[i][1] = load8.y;
      }

      if constexpr (InnerKTiles == 8) {
        // asm volatile("ld.global.cs.v4.u32 {%0, %1, %2, %3}, [%4];\n"
        //              : "=r"(b_int4[i][0]), "=r"(b_int4[i][1]),
        //              "=r"(b_int4[i][2]), "=r"(b_int4[i][3]) : "l"(bPtrCur));

        int4 load16 = reinterpret_cast<const int4*>(bPtrCur)[0];
        b_int4[i][0] = load16.x;
        b_int4[i][1] = load16.y;
        b_int4[i][2] = load16.z;
        b_int4[i][3] = load16.w;
      }
    }

    // Load needed info for dequantization

    static_assert(isPowerOf2(QGroupSize), "");
    static_assert(isEvenDivisor(QGroupSize, kKTileSize), "");
    // smallest quantization group size is 32 (2 k-tiles are packed in an int32)
    static_assert(QGroupSize >= kKTileSize * 2, "");
    constexpr int kKTilesPerQGroup = (QGroupSize / kKTileSize);
    // a q-group could be larger than what we are handling in a single warp
    constexpr int kNumQGroups = (KTilesToLoad / kKTilesPerQGroup) < 1
        ? 1
        : (KTilesToLoad / kKTilesPerQGroup);

    __nv_bfloat162 qScaleAndZero[kNumQGroups];
    {
      int32_t laneN = nTile * kNTileSize + (laneId / 4);
      int32_t groupStart = (kTileStart * kKTileSize) / QGroupSize;

      int32_t n = nTiles * kNTileSize;

      // offset [qScale_kGroup][qScale_n][0]
      auto qInfoPtr = reinterpret_cast<const __nv_bfloat16*>(quantizationInfo) +
          (groupStart * n + laneN) * 2;

#pragma unroll
      for (int i = 0; i < kNumQGroups; ++i) {
        qScaleAndZero[i] =
            *reinterpret_cast<const __nv_bfloat162*>(qInfoPtr + i * n * 2);
      }
    }

    //
    // De-quantize int4 values to bf16. Values are dequantized as truly int4
    // [-8, 7] range; dequant = (bf16(int4_value) * bf16_scale) + bf16_zero
    //
    {
      // FIXME: does this negatively affect register counts, or will nvcc
      // move this expansion (and data loads above) closer to the point of use?
      __nv_bfloat162 qScale[kNumQGroups];
      __nv_bfloat162 qZero[kNumQGroups];

#pragma unroll
      for (int i = 0; i < kNumQGroups; ++i) {
        qScale[i] = __bfloat162bfloat162(qScaleAndZero[i].x);
        qZero[i] = __bfloat162bfloat162(qScaleAndZero[i].y);
      }

#pragma unroll
      for (int i = 0; i < KTilesToLoad / InnerKTiles; ++i) {
#pragma unroll
        for (int j = 0; j < InnerKTiles / 2; ++j) {
          bf16x2x4 v = convert_i4x8_to_bf16x2x4(b_int4[i][j]);

          int curKTile = i * InnerKTiles + j * 2;
          int curQGroup = (curKTile * kKTileSize) / QGroupSize;

          // The dequantized values in `v` for a given lane have the same n
          // dimension (the B tensor core layout has all values in the same
          // thread along the same n) but different k dimension, but all are
          // guaranteed to occur within the same quantization group, so we need
          // only load a single scale + zero to cover what this lane has
#pragma unroll
          for (int k = 0; k < 4; ++k) {
            v.vals[k] = __hfma2(v.vals[k], qScale[curQGroup], qZero[curQGroup]);
          }

          // type pun, the __nv_bfloat162 value in bf16x2x4 is a struct and
          // can't be used as a 32-bit asm register argument for `mma`
          static_assert(sizeof(bf16x2x4) == sizeof(out[0][0]), "");
          std::memcpy(&out[i][j], &v, sizeof(bf16x2x4_u32));
        }
      }
    }
  }
};

template <
    typename ALayout,
    typename BLayout,
    typename CLayout,
    int Warps,
    int KTilesPerIteration>
__global__
__launch_bounds__(Warps* kWarpSize) void tinygemm_m16n8k16_chunk_kernel(
    // Data for the A matrix, loaded as per ALayout
    const void* const __restrict__ A,

    // Data for the B matrix, loaded as per BLayout
    const void* const __restrict__ B,

    // Optional quantization data for dequantizing B, loaded as per BLayout
    const void* const __restrict__ B_quantizationInfo,

    // Output data for the C matrix, stored as per CLayout
    void* __restrict__ C,

    // The size of the matrix multiplication
    int32_t m,
    int32_t n,
    int32_t k,

    // The size of the matrix multiplication, in multiples of our TC tile size
    int32_t mTiles,
    int32_t nTiles,
    int32_t kTiles) {
  constexpr int32_t kMTileSize = 16;
  constexpr int32_t kNTileSize = 8;
  constexpr int32_t kKTileSize = 16;

  static_assert(
      ALayout::kMTileSize == kMTileSize && ALayout::kNTileSize == kNTileSize &&
          ALayout::kKTileSize == kKTileSize,
      "");

  static_assert(
      BLayout::kMTileSize == kMTileSize && BLayout::kNTileSize == kNTileSize &&
          BLayout::kKTileSize == kKTileSize,
      "");

  static_assert(
      CLayout::kMTileSize == kMTileSize && CLayout::kNTileSize == kNTileSize &&
          CLayout::kKTileSize == kKTileSize,
      "");

  constexpr int kInnerKTiles = BLayout::kInnerKTiles;

  // 2/4/8 inner k-tiles correspond to 4, 8 and 16 byte innermost loads
  static_assert(
      kInnerKTiles == 2 || kInnerKTiles == 4 || kInnerKTiles == 8, "");

  // We always process at least kInnerKTiles k-tiles back to back in a warp
  static_assert(
      KTilesPerIteration >= kInnerKTiles &&
          isEvenDivisor(KTilesPerIteration, kInnerKTiles),
      "");

  auto warpId = threadIdx.y;
  auto laneId = threadIdx.x;

  int32_t mTile = blockIdx.z;
  int32_t nTile = blockIdx.y;

  float4 c{0.0f, 0.0f, 0.0f, 0.0f};

  // First, handle whole multiples of KTilesPerIteration
  auto kTilesLimit = roundDown(kTiles, KTilesPerIteration);

  // Each warp handles a set of KTilesPerIteration under the above limit
  for (int32_t kTileBase = (blockIdx.x * Warps + warpId) * KTilesPerIteration;
       kTileBase < kTilesLimit;
       kTileBase += Warps * KTilesPerIteration) {
    //
    // Load data from A
    //
    bf16x2x4_u32 a[KTilesPerIteration];
    ALayout::template load<KTilesPerIteration>(
        A, m, k, mTiles, mTile, kTiles, kTileBase, laneId, a);

    //
    // Load data from B and de-quantize as needed
    // Each k-tile is bf16x2x2
    //
    bf16x2x4_u32 b[KTilesPerIteration / kInnerKTiles][kInnerKTiles / 2];
    BLayout::template load<KTilesPerIteration>(
        B,
        B_quantizationInfo,
        n,
        k,
        nTiles,
        nTile,
        kTiles,
        kTileBase,
        laneId,
        b);

    //
    // Now, perform the matrix multiplication
    //

    // We accumulate across k-tiles here
#pragma unroll
    for (int i = 0; i < KTilesPerIteration / kInnerKTiles; ++i) {
      static_assert(isEvenDivisor(kInnerKTiles, 2) && kInnerKTiles >= 2, "");
#pragma unroll
      for (int j = 0; j < kInnerKTiles / 2; ++j) {
        // We don't simply accumulate into `c` as this creates a too-strong
        // execution dependency. Instead, we only periodically accumulate into
        // `c`
        float4 cTmp[2];

#pragma unroll
        for (int k = 0; k < 2; ++k) {
          cTmp[k] = float4{0.0f, 0.0f, 0.0f, 0.0f};
        }

#pragma unroll
        for (int k = 0; k < 2; ++k) {
          asm volatile(
              "mma.sync.aligned.m16n8k16.row.col.f32.bf16.bf16.f32 "
              "{%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%10,%11,%12,%13};"
              : "=f"(cTmp[k].x),
                "=f"(cTmp[k].y),
                "=f"(cTmp[k].z),
                "=f"(cTmp[k].w)
              : "r"(a[i * kInnerKTiles + j * 2 + k].vals[0]),
                "r"(a[i * kInnerKTiles + j * 2 + k].vals[1]),
                "r"(a[i * kInnerKTiles + j * 2 + k].vals[2]),
                "r"(a[i * kInnerKTiles + j * 2 + k].vals[3]),
                "r"(b[i][(j * 2 + k) / 2].vals[((j * 2 + k) % 2) * 2 + 0]),
                "r"(b[i][(j * 2 + k) / 2].vals[((j * 2 + k) % 2) * 2 + 1]),
                "f"(cTmp[k].x),
                "f"(cTmp[k].y),
                "f"(cTmp[k].z),
                "f"(cTmp[k].w));
        }

#pragma unroll
        for (int k = 0; k < 2; ++k) {
          c.x += cTmp[k].x;
          c.y += cTmp[k].y;
          c.z += cTmp[k].z;
          c.w += cTmp[k].w;
        }
      }
    }
  } // for all tiles under kTilesLimit

  // Now, there could be a remainder of 1 to KTilesPerIteration - 1 k-tiles
  // remaining. We guarantee that the number of warps is >= KTilesPerIteration /
  // kInnerKTiles, so that each warp can simply load kInnerKTiles and do its
  // thing without needing more warps
  static_assert(Warps >= KTilesPerIteration / kInnerKTiles, "");

  auto kTileBaseRemaining = kTilesLimit + warpId * kInnerKTiles;

  // If we have any remainder k-tiles, some warps will handle them, processing
  // kInnerKTiles k-tiles at a time
  if (kTileBaseRemaining < kTiles) {
    bf16x2x4_u32 a[kInnerKTiles];
    ALayout::template load<kInnerKTiles>(
        A, m, k, mTiles, mTile, kTiles, kTileBaseRemaining, laneId, a);

    bf16x2x4_u32 b[1][kInnerKTiles / 2];
    BLayout::template load<kInnerKTiles>(
        B,
        B_quantizationInfo,
        n,
        k,
        nTiles,
        nTile,
        kTiles,
        kTileBaseRemaining,
        laneId,
        b);

#pragma unroll
    for (int j = 0; j < kInnerKTiles / 2; ++j) {
      // We don't simply accumulate into `c` as this creates a too-strong
      // execution dependency. Instead, we only periodically accumulate into
      // `c`
      float4 cTmp[2];

#pragma unroll
      for (int k = 0; k < 2; ++k) {
        cTmp[k] = float4{0.0f, 0.0f, 0.0f, 0.0f};
      }

#pragma unroll
      for (int k = 0; k < 2; ++k) {
        asm volatile(
            "mma.sync.aligned.m16n8k16.row.col.f32.bf16.bf16.f32 "
            "{%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%10,%11,%12,%13};"
            : "=f"(cTmp[k].x), "=f"(cTmp[k].y), "=f"(cTmp[k].z), "=f"(cTmp[k].w)
            : "r"(a[j * 2 + k].vals[0]),
              "r"(a[j * 2 + k].vals[1]),
              "r"(a[j * 2 + k].vals[2]),
              "r"(a[j * 2 + k].vals[3]),
              "r"(b[0][(j * 2 + k) / 2].vals[((j * 2 + k) % 2) * 2 + 0]),
              "r"(b[0][(j * 2 + k) / 2].vals[((j * 2 + k) % 2) * 2 + 1]),
              "f"(cTmp[k].x),
              "f"(cTmp[k].y),
              "f"(cTmp[k].z),
              "f"(cTmp[k].w));
      }

#pragma unroll
      for (int k = 0; k < 2; ++k) {
        c.x += cTmp[k].x;
        c.y += cTmp[k].y;
        c.z += cTmp[k].z;
        c.w += cTmp[k].w;
      }
    }
  }

  //
  // Reduce independent k-tiles (same m/n) across warps
  //
  __shared__ float4 smem_sum[Warps][kWarpSize];

  // FIXME: this likely doesn't need to be a true reduction tree, can just be a
  // serial sum, maybe (unless nvcc/ptxas goes back to its old ways)
  // smem_sum[warpId][laneId] = TreeReduce4<KTilesPerIteration>::reduce(c);
  smem_sum[warpId][laneId] = c;

  __syncthreads();

  if (warpId == 0) {
    float4 sum_f32{0.0f, 0.0f, 0.0f, 0.0f};

    // Reduce across the block in the first warp
    for (int i = 0; i < Warps; ++i) {
      float4 v = smem_sum[i][laneId];
      sum_f32.x += v.x;
      sum_f32.y += v.y;
      sum_f32.z += v.z;
      sum_f32.w += v.w;
    }

    // Write the reduced result (in the first warp) into the output
    CLayout::store(
        C,
        m,
        n,
        mTiles,
        mTile,
        // n for C output becomes k for A input, so for m16n8k16,
        // we need to halve the tiles
        nTiles / 2,
        nTile,
        laneId,
        sum_f32);
  }
}


template <
    typename ALayout,
    typename BLayout,
    typename CLayout,
    int Warps,
    int KTilesPerWarp>
void launch_tinygemm_kernel(
    const at::Tensor& A,
    const at::Tensor& B,
    const at::Tensor* qScaleAndZeros, /* optional */
    at::Tensor& C_final,
    int32_t mTiles,
    int32_t nTiles,
    int32_t kTiles,
    int32_t m,
    int32_t n,
    int32_t k,
    cudaStream_t stream) {
  // The chunking kernel requires that kTiles is a multiple of kInnerKTiles
  TORCH_CHECK(
      kTiles >= BLayout::kInnerKTiles &&
      isEvenDivisor(kTiles, BLayout::kInnerKTiles));

  TORCH_CHECK(
      KTilesPerWarp >= BLayout::kInnerKTiles &&
      isEvenDivisor(KTilesPerWarp, BLayout::kInnerKTiles));

  // After intra-block reduction across the k dimension, we are left with this
  // many tiles
  //  int32_t postKernelKTiles = kTiles / (Warps * KTilesPerWarp);
  int32_t postKernelKTiles = 1; // we loop

  auto grid = dim3(postKernelKTiles, nTiles, mTiles);
  auto block = dim3(kWarpSize, Warps);

  auto func =
      tinygemm_m16n8k16_chunk_kernel<ALayout, BLayout, CLayout, Warps, KTilesPerWarp>;

  func<<<grid, block, 0, stream>>>(
      A.data_ptr(),
      B.data_ptr(),
      qScaleAndZeros ? qScaleAndZeros->data_ptr() : nullptr,
      C_final.data_ptr(),
      m,
      n,
      k,
      mTiles,
      nTiles,
      kTiles);
  C10_CUDA_KERNEL_LAUNCH_CHECK();

  cudaFuncAttributes funcAttr;
  C10_CUDA_CHECK(cudaFuncGetAttributes(
      &funcAttr,
      func));
}

// FIXME: parallelize better, smem staging etc?
template <int InnerKTiles>
__global__ void matrix_to_m16n8k16_Bint4_layout(
    // size [n][k]
    const at::PackedTensorAccessor32<int32_t, 2, at::RestrictPtrTraits> in,
    // size [ceil(n / 8)][ceil(k / (InnerKTiles * 16))][32][InnerKTiles / 2]
    at::PackedTensorAccessor32<int32_t, 4, at::RestrictPtrTraits> out) {
  // int4 values are packed into int32 values, which require at least 8. Given
  // m16n8k16 B layout requires 4 scalar values/lane, the minimum number of
  // innermost k-tiles that we can use is 2.
  static_assert(InnerKTiles >= 2 && isPowerOf2(InnerKTiles), "");

  constexpr int32_t kNTileSize = 8;
  constexpr int32_t kKTileSize = 16;

  // gridDim.x corresponds to the number of k-tiles divided by InnerKTiles
  auto kOuterTile = blockIdx.x;
  auto nTile = blockIdx.y;
  auto t = threadIdx.x;

  // Two k-tiles are packed into an int32 at a time
#pragma unroll
  for (int innerKTile = 0; innerKTile < InnerKTiles; innerKTile += 2) {
    // n dimension that this lane loads from
    auto n0 = nTile * kNTileSize + (t / 4);

    bool n0Valid = n0 < in.size(0);

    int32_t ks[8];

    auto kBase0 = (kOuterTile * InnerKTiles + innerKTile) * kKTileSize;
    ks[0] = kBase0 + (t % 4) * 2;
    ks[1] = ks[0] + 1;
    ks[2] = ks[0] + 8;
    ks[3] = ks[0] + 8 + 1;

    auto kBase1 = kBase0 + kKTileSize;
    ks[4] = kBase1 + (t % 4) * 2;
    ks[5] = ks[4] + 1;
    ks[6] = ks[4] + 8;
    ks[7] = ks[4] + 8 + 1;

    auto pIn = &in[n0][0];

    uint32_t v[8];
#pragma unroll
    for (int i = 0; i < 8; ++i) {
      v[i] = (n0Valid && ks[i] < in.size(1)) ? pIn[ks[i]] : uint32_t(0);
    }

    int32_t pack = (v[7] << 28) | (v[5] << 24) | (v[3] << 20) | (v[1] << 16) |
        (v[6] << 12) | (v[4] << 8) | (v[2] << 4) | v[0];

    // inner k-tiles pack two at a time
    out[nTile][kOuterTile][t][innerKTile / 2] = pack;
  }
}

#endif


at::Tensor _weight_int4pack_mm_cuda(
    const at::Tensor& A,
    const at::Tensor& B,
    int64_t qGroupSize,
    const at::Tensor& qScaleAndZeros) {
  c10::cuda::CUDAGuard g(A.device());

  TORCH_CHECK(
      A.device() == B.device() && A.device() == qScaleAndZeros.device());

  constexpr int32_t kMTileSize = 16;
  constexpr int32_t kNTileSize = 8;
  constexpr int32_t kKTileSize = 16;

  // row major layout
  auto m = A.size(0);
  auto mTiles = divUp(m, kMTileSize);

  // tensor core layout
  auto nTiles = B.size(0);
  auto n = nTiles * kNTileSize;

  // row major layout
  auto k = A.size(1);
  auto kTiles = divUp(k, kKTileSize);

  // The number of inner k tiles is the innermost dimension of  times 2
  // 2 k-tiles (4 values per lane per tile, 8 values total) quantized to int4
  // packed into 1 int32 for int4 B
  auto B_innerKTiles = B.size(3) * 2;
  TORCH_CHECK(B_innerKTiles == 2 || B_innerKTiles == 4 || B_innerKTiles == 8);

  // A is standard row major
  TORCH_CHECK(A.dtype() == at::kBFloat16);
  TORCH_CHECK(A.is_contiguous());
  TORCH_CHECK(A.dim() == 2);

  // B has B_innerKTiles k-tiles in the innermost dimension
  TORCH_CHECK(B.dtype() == at::kInt);
  TORCH_CHECK(B.is_contiguous());
  TORCH_CHECK(B.dim() == 4);
  TORCH_CHECK(B.size(1) == k / (B_innerKTiles * kKTileSize));
  TORCH_CHECK(B.size(2) == kWarpSize);

  // Validate the scale and zero point tensor for dequantization
  // These are the only versions handled at the moment
  TORCH_CHECK(
      qGroupSize == 32 || qGroupSize == 64 || qGroupSize == 128 ||
      qGroupSize == 256);

  TORCH_CHECK(qScaleAndZeros.dim() == 3);
  auto numQGroups = qScaleAndZeros.size(0);
  TORCH_CHECK(
      kTiles * kKTileSize >= qGroupSize &&
      isEvenDivisor(kTiles * kKTileSize, qGroupSize));
  TORCH_CHECK(qScaleAndZeros.size(1) == n);
  TORCH_CHECK(qScaleAndZeros.size(2) == 2);

  // Output is a standard row-major matrix
  auto C_final = at::empty(
      {m, n}, at::TensorOptions().dtype(at::kBFloat16).device(A.device()));

#if (defined(CUDA_VERSION) && CUDA_VERSION >= 12000) && (!defined(__CUDA_ARCH__) || (__CUDA_ARCH__ >= 800))
  auto stream = at::cuda::getCurrentCUDAStream();
#define RUN_GEMM(WARPS, K_TILES_PER_WARP, Q_GROUP_SIZE, REDUCE_TYPE) \
  do {                                                               \
    using ACLayout = ALayout_RM<REDUCE_TYPE>;                        \
                                                                     \
    TORCH_CHECK(                                                     \
        K_TILES_PER_WARP >= B_innerKTiles &&                         \
        isEvenDivisor(K_TILES_PER_WARP, B_innerKTiles));             \
                                                                     \
    switch (B_innerKTiles) {                                         \
      case 2:                                                        \
        if constexpr (K_TILES_PER_WARP >= 2) {                       \
          using BLayout = BLayout_TC_int4<2, Q_GROUP_SIZE>;          \
          launch_tinygemm_kernel<                                    \
              ACLayout,                                              \
              BLayout,                                               \
              ACLayout,                                              \
              WARPS,                                                 \
              K_TILES_PER_WARP>(                                     \
              A,                                                     \
              B,                                                     \
              &qScaleAndZeros,                                       \
              C_final,                                               \
              mTiles,                                                \
              nTiles,                                                \
              kTiles,                                                \
              m,                                                     \
              n,                                                     \
              k,                                                     \
              stream);                                               \
        }                                                            \
        break;                                                       \
      case 4:                                                        \
        if constexpr (K_TILES_PER_WARP >= 4) {                       \
          using BLayout = BLayout_TC_int4<4, Q_GROUP_SIZE>;          \
          launch_tinygemm_kernel<                                    \
              ACLayout,                                              \
              BLayout,                                               \
              ACLayout,                                              \
              WARPS,                                                 \
              K_TILES_PER_WARP>(                                     \
              A,                                                     \
              B,                                                     \
              &qScaleAndZeros,                                       \
              C_final,                                               \
              mTiles,                                                \
              nTiles,                                                \
              kTiles,                                                \
              m,                                                     \
              n,                                                     \
              k,                                                     \
              stream);                                               \
        }                                                            \
        break;                                                       \
      case 8:                                                        \
        if constexpr (K_TILES_PER_WARP >= 8) {                       \
          using BLayout = BLayout_TC_int4<8, Q_GROUP_SIZE>;          \
          launch_tinygemm_kernel<                                    \
              ACLayout,                                              \
              BLayout,                                               \
              ACLayout,                                              \
              WARPS,                                                 \
              K_TILES_PER_WARP>(                                     \
              A,                                                     \
              B,                                                     \
              &qScaleAndZeros,                                       \
              C_final,                                               \
              mTiles,                                                \
              nTiles,                                                \
              kTiles,                                                \
              m,                                                     \
              n,                                                     \
              k,                                                     \
              stream);                                               \
        }                                                            \
        break;                                                       \
      default:                                                       \
        break;                                                       \
    }                                                                \
  } while (false)

#define HANDLE_Q_GROUP(WARPS, K_TILES_PER_WARP, REDUCE_TYPE) \
  do {                                                       \
    switch (qGroupSize) {                                    \
      case 32:                                               \
        RUN_GEMM(WARPS, K_TILES_PER_WARP, 32, REDUCE_TYPE);  \
        break;                                               \
      case 64:                                               \
        RUN_GEMM(WARPS, K_TILES_PER_WARP, 64, REDUCE_TYPE);  \
        break;                                               \
      case 128:                                              \
        RUN_GEMM(WARPS, K_TILES_PER_WARP, 128, REDUCE_TYPE); \
        break;                                               \
      case 256:                                              \
        RUN_GEMM(WARPS, K_TILES_PER_WARP, 256, REDUCE_TYPE); \
        break;                                               \
    }                                                        \
  } while (false)

  HANDLE_Q_GROUP(8, 8, KReductionType::None);

#undef HANDLE_Q_GROUP
#undef RUN_GEMM

  return C_final;
#endif
  TORCH_CHECK(false, "_weight_int4pack_mm_cuda is not available for build.")
  return C_final;
}

// input is [n][k] (int32 dtype)
// output is [n / 8][k / (InnerKTiles * 16)][32][innerKTiles / 2]
at::Tensor _convert_weight_to_int4pack_cuda(
    const at::Tensor& in,
    int64_t innerKTiles) {
  c10::cuda::CUDAGuard g(in.device());

  TORCH_CHECK(in.dim() == 2);
  TORCH_CHECK(in.dtype() == at::kInt);
  TORCH_CHECK(in.is_contiguous());

  // At least 2 k-tiles need to be packed back to back in the innermost
  // dimension, as the m16n8k16 tensor core tile presents 4 scalar values for
  // the B matrix, but the minimum word size for the packed format is 4 bytes
  // (int32). 4 inner K-tiles = 8 byte load, 8 inner k-tiles = 16 byte load
  // which is the maximum vectorized load/store size
  TORCH_CHECK(innerKTiles == 2 || innerKTiles == 4 || innerKTiles == 8);

  constexpr int32_t kNTileSize = 8;
  constexpr int32_t kKTileSize = 16;

  auto nTiles = divUp(in.size(0), kNTileSize);

  // k-tiles are packed back to back in the innermost dimension in order to
  // allow for 4/8/16 byte loads
  TORCH_CHECK(isEvenDivisor(in.size(1), innerKTiles * kKTileSize));
  // kSuperTiles is the number of k-tiles assuming k is innerKTiles * kKTileSize
  auto kSuperTiles = divUp(in.size(1), innerKTiles * kKTileSize);

  // each block handles `innerKTiles` k-tiles.
  // 2 k-tiles are a single int32
  auto out = at::empty(
      {nTiles, kSuperTiles, 32, innerKTiles / 2},
      at::TensorOptions().dtype(at::kInt).device(in.device()));

#if (defined(CUDA_VERSION) && CUDA_VERSION >= 12000) && (!defined(__CUDA_ARCH__) || (__CUDA_ARCH__ >= 800))
  auto stream = at::cuda::getCurrentCUDAStream();
  dim3 grid(kSuperTiles, nTiles);

  if (innerKTiles == 2) {
    matrix_to_m16n8k16_Bint4_layout<2><<<grid, kWarpSize, 0, stream>>>(
        in.packed_accessor32<int32_t, 2, at::RestrictPtrTraits>(),
        out.packed_accessor32<int32_t, 4, at::RestrictPtrTraits>());
  } else if (innerKTiles == 4) {
    matrix_to_m16n8k16_Bint4_layout<4><<<grid, kWarpSize, 0, stream>>>(
        in.packed_accessor32<int32_t, 2, at::RestrictPtrTraits>(),
        out.packed_accessor32<int32_t, 4, at::RestrictPtrTraits>());
  } else if (innerKTiles == 8) {
    matrix_to_m16n8k16_Bint4_layout<8><<<grid, kWarpSize, 0, stream>>>(
        in.packed_accessor32<int32_t, 2, at::RestrictPtrTraits>(),
        out.packed_accessor32<int32_t, 4, at::RestrictPtrTraits>());
  }

  return out;
#endif
  TORCH_CHECK(false, "_convert_weight_to_int4pack_cuda is not available for build.")
  return out;
}


} // namespace at::native
