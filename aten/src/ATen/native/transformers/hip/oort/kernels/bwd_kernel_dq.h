#ifndef OORT_bwd_kernel_dq_H
#define OORT_bwd_kernel_dq_H

namespace oort {

template<int32_t BLOCK_M,
         int32_t BLOCK_DMODEL,
         int32_t BLOCK_N>
struct bwd_kernel_dq {

 hipError_t operator()(dim3 grid, dim3 block, const void* Q,
                       const void* K,
                       const void* V,
                       float sm_scale,
                       const void* Out,
                       const void* DO,
                       const void* DQ,
                       const float* L,
                       const float* D,
                       uint64_t stride_qz,
                       uint64_t stride_qh,
                       uint64_t stride_qm,
                       uint64_t stride_qk,
                       uint64_t stride_kz,
                       uint64_t stride_kh,
                       uint64_t stride_kn,
                       uint64_t stride_kk,
                       uint64_t stride_vz,
                       uint64_t stride_vh,
                       uint64_t stride_vk,
                       uint64_t stride_vn,
                       uint64_t Z,
                       uint64_t H,
                       uint64_t N_CTX, hipStream_t stream);

};


template struct bwd_kernel_dq<128 /* BLOCK_M */,
                              16 /* BLOCK_DMODEL */,
                              64 /* BLOCK_N */>;
template struct bwd_kernel_dq<128 /* BLOCK_M */,
                              32 /* BLOCK_DMODEL */,
                              64 /* BLOCK_N */>;
template struct bwd_kernel_dq<128 /* BLOCK_M */,
                              64 /* BLOCK_DMODEL */,
                              64 /* BLOCK_N */>;
template struct bwd_kernel_dq<128 /* BLOCK_M */,
                              128 /* BLOCK_DMODEL */,
                              64 /* BLOCK_N */>;
}; // namespace oort

#endif

