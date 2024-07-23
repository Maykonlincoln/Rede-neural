# mypy: allow-untyped-defs
import dataclasses
from enum import Enum
from typing import Callable, Dict, List, Optional, Type

import sympy

import torch

from .. import ir
from ..cpu_vec_isa import pick_vec_isa, VecAMX, VecAVX2, VecAVX512, VecISA
from ..utils import IndentedBuffer, parallel_num_threads
from ..virtualized import V
from .common import KernelTemplate
from .cpp_template_kernel import CppTemplateKernel
from .cpp_utils import DTYPE_TO_CPP, GemmBlocking, value_to_cpp


class LayoutType(Enum):
    NORMAL = 0
    VNNI2 = 1
    VNNI4 = 2


class CppMicroGemm:
    """
    A class that codegens a kernel that computes small-sized matrix multiplication.

    A micro GEMM kernel is responsible for register blocking, instruction selection,
    and other CPU architecture-specific optimizations.

    The subclasses need to override `codegen_define` to define the kernel function
    that is called by the code generated by `codegen_call`.
    """

    # TODO(jgong5): support constant shapes and lds as template args.
    DECLARE_KERNEL = r"""
template <bool accum>
inline void {{kernel_name}}(
{%- if kernel_extra_args_declare %}
    {{kernel_extra_args_declare}}
{%- endif %}
    const {{input_t}}* __restrict__ A,
    const {{input2_t}}* __restrict__ B,
    {{output_t}}* __restrict__ C,
    int64_t M,
    int64_t N,
    int64_t K,
    int64_t lda,
    int64_t ldb,
    int64_t ldc
)
"""

    def __init__(
        self,
        name,
        input_dtype,
        input2_dtype,
        output_dtype,
        compute_dtype,
        register_blocking,
        alpha=1,
    ):
        self.name = name
        self.input_dtype = input_dtype
        assert input2_dtype is not None
        self.input2_dtype = input2_dtype
        self.output_dtype = output_dtype
        self.compute_dtype = compute_dtype
        self.register_blocking = register_blocking
        self.alpha = alpha

    def get_common_options(self):
        if self.input_dtype == torch.uint8:
            assert self.compute_dtype == torch.int32
            assert self.output_dtype == torch.int32
            assert self.input2_dtype == torch.int8
        return {
            "torch": torch,
            "kernel_name": self.name,
            "input_dtype": self.input_dtype,
            "output_dtype": self.output_dtype,
            "compute_dtype": self.compute_dtype,
            "input_t": DTYPE_TO_CPP[self.input_dtype],
            "input2_t": DTYPE_TO_CPP[self.input2_dtype],
            "output_t": DTYPE_TO_CPP[self.output_dtype],
            "compute_t": DTYPE_TO_CPP[self.compute_dtype],
            "alpha": self.alpha,
            "kernel_extra_args_declare": self.get_kernel_extra_args_declare(),
            "int8_gemm": self.input_dtype == torch.uint8,
            "vnni_size": 4 if self.input_dtype == torch.uint8 else 2,
        }

    def get_kernel_declaration(self):
        options = self.get_common_options()
        return KernelTemplate._template_from_string(self.DECLARE_KERNEL).render(options)

    def get_kernel_extra_args_declare(self) -> str:
        return ""

    def get_kernel_extra_args(self) -> str:
        return ""

    def codegen_define(self, kernel: CppTemplateKernel) -> str:
        raise NotImplementedError

    def codegen_call(
        self,
        kernel: CppTemplateKernel,
        A: ir.Buffer,
        B: ir.Buffer,
        C: ir.Buffer,
        accum: bool,
    ) -> str:
        """
        Generate the code for calling the templated kernel that computes
        `C += alpha * A @ B` if `accum` is True, or `C = alpha * A @ B` otherwise.
        """
        A_ptr = f"&({kernel.index(A, [0, 0])})"
        B_ptr = f"&({kernel.index(B, [0, 0])})"
        C_ptr = f"&({kernel.index(C, [0, 0])})"
        M = kernel.size(C, 0)
        N = kernel.size(C, 1)
        K = kernel.size(A, 1)
        lda = kernel.stride(A, 0)
        ldb = kernel.stride(B, 0)
        ldc = kernel.stride(C, 0)
        res = IndentedBuffer()
        res.writeline(f"{self.name}<{value_to_cpp(accum, 'bool')}>(")
        with res.indent():
            extra_args = self.get_kernel_extra_args()
            if extra_args:
                res.writeline(extra_args)
            res.writeline(f"{A_ptr},")
            res.writeline(f"{B_ptr},")
            res.writeline(f"{C_ptr},")
            res.writeline(f"{M},")
            res.writeline(f"{N},")
            res.writeline(f"{K},")
            res.writeline(f"{lda},")
            res.writeline(f"{ldb},")
            res.writeline(f"{ldc}")
        res.writeline(");")
        return res.getvalue()

    def codegen_init(
        self,
        kernel: CppTemplateKernel,
    ) -> str:
        return ""

    def codegen_finalize(
        self,
        kernel: CppTemplateKernel,
    ) -> str:
        return ""

    def get_b_layout(self) -> LayoutType:
        return LayoutType.NORMAL


@dataclasses.dataclass
class CppMicroGemmConfig:
    input_dtype: torch.dtype
    input2_dtype: torch.dtype
    output_dtype: torch.dtype
    compute_dtype: torch.dtype
    vec_isa_cls: Type[VecISA]
    register_blocking: GemmBlocking
    extra_check: Optional[Callable[..., bool]] = None


micro_gemm_configs: Dict[Type[CppMicroGemm], List[CppMicroGemmConfig]] = {}


def register_micro_gemm(*configs):
    def inner(cls):
        assert (
            cls not in micro_gemm_configs
        ), f"Duplicate micro_gemm registration for {cls}"
        assert len(configs) > 0, f"No micro_gemm configs provided for {cls}"
        micro_gemm_configs[cls] = list(configs)
        return cls

    return inner


def generate_gemm_config(
    vec_isa_cls,
    register_blockings,
    input_dtype=torch.float,
    input2_dtype=None,
    output_dtype=None,
    compute_dtype=None,
    extra_check=None,
):
    if output_dtype is None:
        output_dtype = input_dtype
    if compute_dtype is None:
        compute_dtype = output_dtype
    if input2_dtype is None:
        input2_dtype = input_dtype
    return [
        CppMicroGemmConfig(
            input_dtype,
            input2_dtype,
            output_dtype,
            compute_dtype,
            vec_isa_cls,
            GemmBlocking(*blocking),
            extra_check,
        )
        for blocking in register_blockings
    ]


class CppMicroGemmRef(CppMicroGemm):
    """
    A reference implementation of the CppMicroGemm class with naive C++ code.
    It is used for correctness debugging.
    """

    TEMPLATE_ENTRY = r"""
{{declare_kernel}} {
    for (int64_t m = 0; m < M; ++m) {
        for (int64_t n = 0; n < N; ++n) {
            {{compute_t}} result = accum ? C[m * ldc + n] : 0;
            for (int64_t k = 0; k < K; ++k) {
                result += ({{compute_t}})A[m * lda + k] * ({{compute_t}})B[k * ldb + n] * {{alpha}};
            }
            C[m * ldc + n] = result;
        }
    }
}
"""

    def __init__(
        self, name, input_dtype, input2_dtype, output_dtype, compute_dtype, alpha
    ):
        super().__init__(
            name,
            input_dtype,
            input2_dtype,
            output_dtype,
            compute_dtype,
            GemmBlocking(1, 1, 1),
            alpha,
        )

    def codegen_define(self, kernel: CppTemplateKernel) -> str:
        options = {
            "declare_kernel": self.get_kernel_declaration(),
            **self.get_common_options(),
        }
        return KernelTemplate._template_from_string(self.TEMPLATE_ENTRY).render(options)


@register_micro_gemm(
    *generate_gemm_config(
        VecAVX512,
        [(8, 48, 1), (8, 32, 1), (16, 16, 1)],
        input_dtype=torch.float,
    ),
    *generate_gemm_config(
        VecAVX512,
        [(8, 48, 1), (8, 32, 1), (16, 16, 1)],
        input_dtype=torch.bfloat16,
        output_dtype=torch.float,
    ),
    *generate_gemm_config(
        VecAVX512,
        [(8, 48, 1), (8, 32, 1), (16, 16, 1)],
        input_dtype=torch.half,
        output_dtype=torch.float,
    ),
    *generate_gemm_config(
        VecAVX2,
        [(4, 24, 1), (4, 16, 1), (8, 8, 1)],
        input_dtype=torch.float,
    ),
    *generate_gemm_config(
        VecAVX2,
        [(4, 24, 1), (4, 16, 1), (8, 8, 1)],
        input_dtype=torch.bfloat16,
        output_dtype=torch.float,
    ),
    *generate_gemm_config(
        VecAVX2,
        [(4, 24, 1), (4, 16, 1), (8, 8, 1)],
        input_dtype=torch.half,
        output_dtype=torch.float,
    ),
)
class CppMicroGemmFP32Vec(CppMicroGemm):
    """
    This class generates the code for micro gemm using fp32 vec instructions for compute.
    It supports input types of torch.float, torch.bfloat16, and torch.half with fp32 output.
    """

    TEMPLATE_ENTRY = r"""
{{declare_kernel}} {
    {{kernel.assert_function}}(N % {{block_n}} == 0, "N dimension must be multiple of {{block_n}}");
    {{kernel.assert_function}}(K % {{block_k}} == 0, "K dimension must be multiple of {{block_k}}");
    // TODO(jgong5): loop unroll for M and N
    for (int64_t m = 0; m < M; m += {{block_m}}) {
        int64_t block_m = std::min<int64_t>(M - m, {{block_m}});
        for (int64_t n = 0; n < N; n += {{block_n}}) {
            if (block_m == {{block_m}}) {
                {{kernel_name}}_kernel<{{block_m}}, {{block_n}}, accum>(
                    A + m * lda,
                    B + n,
                    C + m * ldc + n,
                    K,
                    lda,
                    ldb,
                    ldc
                );
            } else {
                switch (block_m) {
                {%- for b in range(block_m - 1, 0, -1) %}
                case {{b}}:
                    {{kernel_name}}_kernel<{{b}}, {{block_n}}, accum>(
                        A + m * lda,
                        B + n,
                        C + m * ldc + n,
                        K,
                        lda,
                        ldb,
                        ldc
                    );
                    break;
                {%- endfor %}
                default:
                    {{kernel.assert_function}}(false, "Unsupported block_m");
                }
            }
        }
    }
}
"""

    TEMPLATE_KERNEL = r"""
template <int64_t BLOCK_M, int64_t BLOCK_N, bool accum>
inline void {{kernel_name}}_kernel(
    const {{input_t}}* __restrict__ A,
    const {{input_t}}* __restrict__ B,
    {{output_t}}* __restrict__ C,
    int64_t K,
    int64_t lda,
    int64_t ldb,
    int64_t ldc
) {
    using Vectorized = at::vec::Vectorized<{{compute_t}}>;
    using VectorizedIn = at::vec::Vectorized<{{input_t}}>;
    constexpr auto VLEN = Vectorized::size();
    constexpr auto ROWS = BLOCK_M;
    constexpr auto COLS = BLOCK_N / VLEN;

    Vectorized va;
    at::vec::VectorizedN<{{compute_t}}, COLS> vb;
    at::vec::VectorizedN<{{compute_t}}, ROWS*COLS> vc;

    auto loadc = [&](auto i) {
        if constexpr (accum) {
            constexpr int row = i / COLS;
            constexpr int col = i % COLS;
            vc[i] = Vectorized::loadu(C + row * ldc + col * VLEN);
        } else {
            vc[i] = Vectorized(0.0f);
        }
    };
    c10::ForcedUnroll<ROWS * COLS>{}(loadc);

    auto compute = [&, COLS](auto i, int k) {
        constexpr int row = i / COLS;
        constexpr int col = i % COLS;

        if constexpr (col == 0) {
            {%- if alpha != 1 %}
            va = Vectorized(static_cast<{{compute_t}}>(A[row * lda + k]) * {{alpha}});
            {%- else %}
            va = Vectorized(static_cast<{{compute_t}}>(A[row * lda + k]));
            {%- endif %}
        }

        if constexpr (row == 0) {
            {%- if input_dtype == torch.bfloat16 or input_dtype == torch.float16 %}
            auto b = VectorizedIn::loadu(B + k * ldb + col * VLEN, VLEN);
            vb[col] = at::vec::convert<{{compute_t}}>(b);
            {%- else %}
            vb[col] = Vectorized::loadu(B + k * ldb + col * VLEN);
            {%- endif %}
        }

        constexpr int idx = row * COLS + col;
        vc[idx] = at::vec::fmadd(va, vb[col], vc[idx]);
    };

    {{kernel.unroll_pragma(4)}}
    for (int k = 0; k < K; ++k) {
        c10::ForcedUnroll<ROWS * COLS>{}(compute, k);
    }

    // store to C
    auto storec = [&](auto i) {
        constexpr int row = i / COLS;
        constexpr int col = i % COLS;
        vc[i].store(C + row * ldc + col * VLEN);
    };
    c10::ForcedUnroll<ROWS * COLS>{}(storec);
}
"""

    def codegen_define(self, kernel: CppTemplateKernel) -> str:
        options = {
            "declare_kernel": self.get_kernel_declaration(),
            "kernel": kernel,
            "block_m": self.register_blocking.block_m,
            "block_n": self.register_blocking.block_n,
            "block_k": self.register_blocking.block_k,
            **self.get_common_options(),
        }
        result = KernelTemplate._template_from_string(self.TEMPLATE_KERNEL).render(
            options
        )
        result += KernelTemplate._template_from_string(self.TEMPLATE_ENTRY).render(
            options
        )
        return result


# extra check for CppMicroGemmAMX
def check_amx_extra(config, m, n, k, alpha, num_threads):
    vnni_size = 4 if config.input_dtype == torch.uint8 else 2
    return k % vnni_size == 0 and alpha == 1


@register_micro_gemm(
    *generate_gemm_config(
        VecAMX,
        [(32, 32, 32), (48, 16, 32), (16, 48, 32)],
        input_dtype=torch.bfloat16,
        output_dtype=torch.float,
        extra_check=check_amx_extra,
    ),
    *generate_gemm_config(
        VecAMX,
        [(32, 32, 64), (48, 16, 64)],
        input_dtype=torch.uint8,
        input2_dtype=torch.int8,
        output_dtype=torch.int32,
        compute_dtype=torch.int32,
        extra_check=check_amx_extra,
    ),
)
class CppMicroGemmAMX(CppMicroGemm):
    """
    This class generates the code for micro gemm using Advanced Matrix eXtention (AMX)
    instructions available in 4th generation Intel Xeon for compute.
    It supports input types of torch.bfloat16 with fp32 output.
    TODO(jgong5): support int8 data type.
    """

    TEMPLATE_ENTRY = r"""
{{declare_kernel}} {
    {{kernel.assert_function}}(N % {{block_n}} == 0, "N dimension must be multiple of {{block_n}}");
    {{kernel.assert_function}}(K % 2 == 0, "K dimension must be multiple of 2");
    // TODO(jgong5): loop unroll for M and N
    for (int64_t m = 0; m < M; m += {{block_m}}) {
        int64_t block_m = std::min<int64_t>(M - m, {{block_m}});
        int64_t m_tail = m;
        for (int64_t n = 0; n < N; n += {{block_n}}) {
            {%- for num_rows in range(block_m, 0, -16) %}
            {%- if num_rows != block_m %}
            else
            {%- endif %}
            if (block_m >= {{num_rows}}) {
                {{kernel_name}}_amx_kernel_{{num_rows}}_{{num_columns}}<accum>(
                    amx_state,
                    A + m * lda,
                    B + n,
                    C + m * ldc + n,
                    K,
                    lda,
                    ldb,
                    ldc,
                    16
                );
                block_m -= {{num_rows}};
                m_tail += {{num_rows}};
            }
            {%- endfor %}
            if (block_m > 0) {
                {{kernel_name}}_amx_kernel_16_{{num_columns}}<accum>(
                    amx_state,
                    A + m_tail * lda,
                    B + n,
                    C + m_tail * ldc + n,
                    K,
                    lda,
                    ldb,
                    ldc,
                    block_m
                );
            }
        }
    }
}
"""

    TEMPLATE_KERNEL = r"""
template <bool accum>
inline void {{kernel_name}}_amx_kernel_{{num_rows}}_{{num_columns}}(
    AMXState& amx_state,
    const {{input_t}}* __restrict__ A,
    const {{input2_t}}* __restrict__ B,
    {{output_t}}* __restrict__ C,
    int64_t K,
    int64_t lda,
    int64_t ldb,
    int64_t ldc,
    uint8_t tilecfg_rows
) {
    // TODO(jgong5): add prefetch hint for A, B, C
    auto loadconfig = [](const amx_tilecfg& cfg) {
        _tile_loadconfig(&cfg);
    };
    const auto last_k_offset = K / {{block_k}} * {{block_k}};
    const auto tail_k_size = K - last_k_offset;
    if C10_LIKELY (last_k_offset > 0) {
        amx_state.configure(tilecfg_rows, 64, {{num_rows}} / 16, {{num_columns}}, loadconfig);
    } else {
        amx_state.configure(tilecfg_rows, tail_k_size * sizeof({{input_t}}), {{num_rows}} / 16, {{num_columns}}, loadconfig);
    }
    auto load_c = [&]() {
    {%- for tile_row in range(num_rows // 16) %}
        {%- for tile_col in range(num_columns) %}
        {%- set tile_idx = tile_row * num_columns + tile_col %}
        _tile_loadd({{tile_idx}}, C + {{tile_row * 16}} * ldc + {{tile_col * 16}}, ldc * sizeof({{output_t}}));
        {%- endfor %}
    {%- endfor %}
    };
    auto zero_c = [&]() {
    {%- for tile_row in range(num_rows // 16) %}
        {%- for tile_col in range(num_columns) %}
        {%- set tile_idx = tile_row * num_columns + tile_col %}
        _tile_zero({{tile_idx}});
        {%- endfor %}
    {%- endfor %}
    };

    if constexpr (accum) {
        load_c();
    } else {
        zero_c();
    }

    auto compute = [&](int k) {
    {%- set tile_offset_a = num_rows // 16 * num_columns %}
    {%- set tile_offset_b = tile_offset_a + num_rows // 16 %}
    {%- for tile_row in range(num_rows // 16) %}
        {%- for tile_col in range(num_columns) %}
        {%- set tile_idx_a = tile_offset_a + tile_row %}
        {%- set tile_idx_b = tile_offset_b + tile_col %}
        {%- set tile_idx_c = tile_row * num_columns + tile_col %}
        {%- if tile_col == 0 %}
        _tile_stream_loadd({{tile_idx_a}}, A + {{tile_row * 16}} * lda + k, lda * sizeof({{input_t}}));
        {%- endif %}
        {%- if tile_row == 0 %}
        _tile_loadd({{tile_idx_b}}, B + k * ldb + {{tile_col * 16 * vnni_size}}, ldb * {{vnni_size}} * sizeof({{input_t}}));
        {%- endif %}
        {%- if int8_gemm %}
        _tile_dpbusd({{tile_idx_c}}, {{tile_idx_a}}, {{tile_idx_b}});
        {%- else %}
        _tile_dpbf16ps({{tile_idx_c}}, {{tile_idx_a}}, {{tile_idx_b}});
        {%- endif %}
        {%- endfor %}
    {%- endfor %}
    };

    {{kernel.unroll_pragma(4)}}
    for (int k = 0; k < last_k_offset; k += {{block_k}}) {
        compute(k);
    }

    auto store_c = [&]() {
    // store to C
    {%- for tile_row in range(num_rows // 16) %}
        {%- for tile_col in range(num_columns) %}
        {%- set tile_idx = tile_row * num_columns + tile_col %}
        _tile_stored({{tile_idx}}, C + {{tile_row * 16}} * ldc + {{tile_col * 16}}, ldc * sizeof({{output_t}}));
        {%- endfor %}
    {%- endfor %}
    };

    // TODO(jgong5): move tail k computation to separate loopnest to save tile configuration overhead
    if C10_UNLIKELY (tail_k_size > 0) {
        if C10_LIKELY (last_k_offset > 0) {
            store_c();
            amx_state.configure(tilecfg_rows, tail_k_size * sizeof({{input_t}}), {{num_rows}} / 16, {{num_columns}}, loadconfig);
            load_c();
        }
        compute(last_k_offset);
    }

    store_c();
}
"""

    def codegen_define(self, kernel: CppTemplateKernel) -> str:
        block_m, block_n, block_k = self.register_blocking
        assert block_m % 16 == 0, "Only support block_m % 16 == 0 for AMX"
        assert block_n % 16 == 0, "Only support block_n % 16 == 0 for AMX"
        if self.input_dtype == torch.uint8:
            assert block_k == 64, "Only support block_k = 64 for AMX INT8"
        else:
            assert block_k == 32, "Only support block_k = 32 for AMX Bfloat16/Float16"
        num_columns = block_n // 16
        options = {
            "declare_kernel": self.get_kernel_declaration(),
            "kernel": kernel,
            "block_m": block_m,
            "block_n": block_n,
            "block_k": block_k,
            "num_columns": num_columns,
            **self.get_common_options(),
        }
        result = ""
        for num_rows in range(block_m, 0, -16):
            amx_kernel_options = {**options, "num_rows": num_rows}
            result += KernelTemplate._template_from_string(self.TEMPLATE_KERNEL).render(
                amx_kernel_options
            )
        result += KernelTemplate._template_from_string(self.TEMPLATE_ENTRY).render(
            options
        )
        return result

    def codegen_init(
        self,
        kernel: CppTemplateKernel,
    ) -> str:
        return "AMXState amx_state;"

    def codegen_finalize(
        self,
        kernel: CppTemplateKernel,
    ) -> str:
        return "amx_state.release([]() { _tile_release(); });"

    def get_kernel_extra_args_declare(self) -> str:
        return "AMXState& amx_state,"

    def get_kernel_extra_args(self) -> str:
        return "amx_state,"

    def get_b_layout(self):
        if self.input_dtype == torch.uint8:
            return LayoutType.VNNI4
        else:
            return LayoutType.VNNI2


def create_micro_gemm(
    name,
    m,
    n,
    k,
    input_dtype,
    input2_dtype,
    output_dtype=None,
    compute_dtype=None,
    alpha=1,
    num_threads=-1,
    use_ref=True,
) -> Optional[CppMicroGemm]:
    def create_from_config(cls, config: CppMicroGemmConfig):
        return cls(
            name,
            config.input_dtype,
            config.input2_dtype,
            config.output_dtype,
            config.compute_dtype,
            config.register_blocking,
            alpha,
        )

    assert isinstance(n, int) or n.is_number, n
    assert isinstance(k, int) or k.is_number, k
    m = V.graph.sizevars.size_hint(m, fallback=1) if isinstance(m, sympy.Expr) else m
    assert isinstance(m, int), m
    if output_dtype is None:
        output_dtype = input_dtype
    if compute_dtype is None:
        compute_dtype = output_dtype
    if num_threads < 0:
        num_threads = parallel_num_threads()
    vec_isa = pick_vec_isa()
    matched_configs = []
    for cls, configs in micro_gemm_configs.items():
        for config in configs:
            if not issubclass(vec_isa.__class__, config.vec_isa_cls):
                continue
            if (
                config.input_dtype == input_dtype
                and config.output_dtype == output_dtype
                and config.compute_dtype == compute_dtype
                and config.input2_dtype == input2_dtype
            ):
                if config.extra_check is not None and not config.extra_check(
                    config, m, n, k, alpha, num_threads
                ):
                    continue
                block_m, block_n, block_k = config.register_blocking
                # Criteria on the ranking of configurations
                # 1. ISA: AMX > VEC
                # 2. Dividable by block sizes (block_m, block_n, block_k)
                # 3. Number of mxn blocks is large enough to occupy all the threads
                # 4. Register blocks are larger
                isa_score = 0
                if config.vec_isa_cls == VecAMX:
                    isa_score += 1
                dividable_score = 0
                if m % block_m == 0:
                    dividable_score += 1
                if n % block_n == 0:
                    dividable_score += 1
                if k % block_k == 0:
                    dividable_score += 1
                occupancy_score = 0
                n_blocks = (n + block_n - 1) // block_n
                total_mxn_blocks = n_blocks * ((m + block_m - 1) // block_m)
                if n_blocks >= num_threads:
                    occupancy_score += 1
                if total_mxn_blocks >= num_threads:
                    occupancy_score += 1
                register_bytes = (
                    block_m * block_n * config.compute_dtype.itemsize
                    + (block_m * block_k + block_k * block_n)
                    * config.input_dtype.itemsize
                )
                matched_configs.append(
                    (
                        (isa_score, dividable_score, occupancy_score, register_bytes),
                        cls,
                        config,
                    )
                )
    if len(matched_configs) == 0:
        if use_ref:
            return CppMicroGemmRef(
                name, input_dtype, input2_dtype, output_dtype, compute_dtype, alpha
            )
        else:
            return None
    # TODO(jgong5): allow autotuning on choices of configs
    return create_from_config(*max(matched_configs, key=lambda x: x[0])[1:])
