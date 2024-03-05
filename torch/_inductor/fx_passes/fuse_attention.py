import functools
import inspect
import logging
import math

import torch
from ..._dynamo.utils import counters
from ..compile_fx import IS_AVX512_BF16_SUPPORTED
from ..pattern_matcher import (
    filter_nodes,
    fwd_only,
    joint_fwd_bwd,
    register_replacement,
)

log = logging.getLogger(__name__)
aten = torch.ops.aten


def _sfdp_pattern_1(query, key, value, inv_scale):
    return (
        torch.matmul(query, key.transpose(-2, -1))
        .div(inv_scale)
        .softmax(dim=-1)
        .matmul(value)
    )


def _sfdp_replacement_1(query, key, value, inv_scale):
    counters["inductor"]["fuse_attention"] += 1
    return aten.scaled_dot_product_attention(
        query.contiguous(),
        key.contiguous(),
        value.contiguous(),
        attn_mask=None,
        dropout_p=0.0,
        is_causal=False,
        scale=1.0 / inv_scale,
    )


def _sfdp_pattern_2(query, key, value, scale_factor):
    return (
        torch.matmul(query, key.transpose(-2, -1))
        .mul(scale_factor)
        .softmax(dim=-1)
        .matmul(value)
    )


def _sfdp_replacement_2(query, key, value, scale_factor):
    counters["inductor"]["fuse_attention"] += 1
    return aten.scaled_dot_product_attention(
        query.contiguous(),
        key.contiguous(),
        value.contiguous(),
        attn_mask=None,
        dropout_p=0.0,
        is_causal=False,
        scale=scale_factor,
    )


def _sfdp_pattern_3(query, key, value, inv_scale_factor, dropout_p):
    return torch.nn.functional.dropout(
        torch.matmul(query, key.transpose(-2, -1))
        .div(inv_scale_factor)
        .softmax(dim=-1),
        p=dropout_p,
    ).matmul(value)


def _sfdp_replacement_3(query, key, value, inv_scale_factor, dropout_p):
    counters["inductor"]["fuse_attention"] += 1
    return aten.scaled_dot_product_attention(
        query.contiguous(),
        key.contiguous(),
        value.contiguous(),
        attn_mask=None,
        dropout_p=dropout_p,
        is_causal=False,
        scale=1.0 / inv_scale_factor,
    )


def _sfdp_pattern_4(query, key, value, scale_factor, dropout_p):
    return torch.nn.functional.dropout(
        torch.matmul(query, key.transpose(-2, -1)).mul(scale_factor).softmax(dim=-1),
        p=dropout_p,
    ).matmul(value)


def _sfdp_replacement_4(query, key, value, scale_factor, dropout_p):
    counters["inductor"]["fuse_attention"] += 1
    return aten.scaled_dot_product_attention(
        query.contiguous(),
        key.contiguous(),
        value.contiguous(),
        attn_mask=None,
        dropout_p=dropout_p,
        is_causal=False,
        scale=scale_factor,
    )


def _sfdp_pattern_5(query, key, value, attn_mask):
    attn_weight = torch.softmax(
        (query @ key.transpose(-2, -1) / math.sqrt(query.size(-1))) + attn_mask, dim=-1
    )
    # attn_weight = torch.dropout(attn_weight, dropout_p)
    return attn_weight @ value


def _sfdp_replacement_5(query, key, value, attn_mask):
    counters["inductor"]["fuse_attention"] += 1
    return aten.scaled_dot_product_attention(
        query.contiguous(),
        key.contiguous(),
        value.contiguous(),
        attn_mask=attn_mask.to(dtype=query.dtype),
        dropout_p=0.0,
        is_causal=False,
    )


def _sfdp_pattern_6(query, key, value, attn_mask, dropout_p):
    attn_weight = torch.softmax(
        (query @ key.transpose(-2, -1) / math.sqrt(query.size(-1))) + attn_mask, dim=-1
    )
    attn_weight = torch.dropout(attn_weight, dropout_p, True)
    return attn_weight @ value


def _sfdp_replacement_6(query, key, value, attn_mask, dropout_p):
    counters["inductor"]["fuse_attention"] += 1
    return aten.scaled_dot_product_attention(
        query.contiguous(),
        key.contiguous(),
        value.contiguous(),
        attn_mask=attn_mask.to(dtype=query.dtype),
        dropout_p=dropout_p,
        is_causal=False,
    )


def _sfdp_pattern_7(query, key, value, dropout_p):
    # in real workloads inputs to matmul are permuted
    # causing matmul to expand to a series of expand and clone calls
    # we want the same to happen during pattern tracing
    q = query.permute(0, 2, 1, 3)
    k = key.permute(0, 2, 1, 3)
    v = value.permute(0, 2, 1, 3)
    div = q @ k.transpose(-2, -1) / math.sqrt(q.size(-1))
    div = div.to(torch.float32)
    attn_weight = torch.softmax(div, dim=-1)
    attn_weight = torch.dropout(attn_weight, dropout_p, True)
    attn_weight = attn_weight.to(torch.float16)
    return attn_weight @ v


def _sfdp_replacement_7(query, key, value, dropout_p):
    # sdpa prefers inputs in permuted format
    # it makes a copy to put them in this format
    # if they aren't already
    # to make replacement efficient ensure that inputs to sdpa
    # are in required order
    counters["inductor"]["fuse_attention"] += 1
    q = query.permute(0, 2, 1, 3)
    k = key.permute(0, 2, 1, 3)
    v = value.permute(0, 2, 1, 3)
    return aten.scaled_dot_product_attention(
        q,
        k,
        v,
        attn_mask=None,  # attn_mask,
        dropout_p=dropout_p,
        is_causal=False,
    )


def _sfdp_pattern_8(query, key, value):
    # no dropout version of pattern 7
    q = query.permute(0, 2, 1, 3)
    k = key.permute(0, 2, 1, 3)
    v = value.permute(0, 2, 1, 3)
    div = q @ k.transpose(-2, -1) / math.sqrt(q.size(-1))
    div = div.to(torch.float32)
    attn_weight = torch.softmax(div, dim=-1)
    attn_weight = attn_weight.to(torch.float16)
    return attn_weight @ v


def _sfdp_replacement_8(query, key, value):
    counters["inductor"]["fuse_attention"] += 1
    q = query.permute(0, 2, 1, 3)
    k = key.permute(0, 2, 1, 3)
    v = value.permute(0, 2, 1, 3)
    return aten.scaled_dot_product_attention(
        q,
        k,
        v,
        attn_mask=None,  # attn_mask,
        dropout_p=0.0,
        is_causal=False,
    )


def _sfdp_pattern_9(query, key, value, dropout_p):
    q = query.permute(0, 2, 1, 3)
    k = key.permute(0, 2, 1, 3)
    v = value.permute(0, 2, 1, 3)
    q = q / math.sqrt(q.size(-1))
    div = q @ k.transpose(-2, -1)
    div = div.to(torch.float32)
    attn_weight = torch.softmax(div, dim=-1)
    attn_weight = torch.dropout(attn_weight, dropout_p, True)
    attn_weight = attn_weight.to(torch.float16)
    return attn_weight @ v


def _sfdp_replacement_9(query, key, value, dropout_p):
    counters["inductor"]["fuse_attention"] += 1
    q = query.permute(0, 2, 1, 3)
    k = key.permute(0, 2, 1, 3)
    v = value.permute(0, 2, 1, 3)
    return aten.scaled_dot_product_attention(
        q,
        k,
        v,
        attn_mask=None,  # attn_mask,
        dropout_p=dropout_p,
        is_causal=False,
    )


def _sfdp_pattern_10(query, key, value):
    # no dropout version of 9
    q = query.permute(0, 2, 1, 3)
    k = key.permute(0, 2, 1, 3)
    v = value.permute(0, 2, 1, 3)
    q = q / math.sqrt(q.size(-1))
    div = q @ k.transpose(-2, -1)
    div = div.to(torch.float32)
    attn_weight = torch.softmax(div, dim=-1)
    attn_weight = attn_weight.to(torch.float16)
    return attn_weight @ v


def _sfdp_replacement_10(query, key, value):
    counters["inductor"]["fuse_attention"] += 1
    q = query.permute(0, 2, 1, 3)
    k = key.permute(0, 2, 1, 3)
    v = value.permute(0, 2, 1, 3)
    return aten.scaled_dot_product_attention(
        q,
        k,
        v,
        attn_mask=None,  # attn_mask,
        dropout_p=0.0,
        is_causal=False,
    )


def _sfdp_pattern_11(query, key, value, inv_scale):
    # Mainly for huggingface models
    q = query.permute(0, 2, 1, 3)
    k = key.permute(0, 2, 1, 3)
    v = value.permute(0, 2, 1, 3)
    return torch.matmul(q, k.transpose(-2, -1)).div(inv_scale).softmax(dim=-1).matmul(v)


def _sfdp_replacement_11(query, key, value, inv_scale):
    counters["inductor"]["fuse_attention"] += 1
    return aten.scaled_dot_product_attention(
        query.transpose(1, 2),
        key.transpose(1, 2),
        value.transpose(1, 2),
        attn_mask=None,
        dropout_p=0.0,
        is_causal=False,
        scale=1.0 / inv_scale,
    )


def _sfdp_pattern_12(query, key, value, inv_scale_factor, dropout_p):
    q = query.permute(0, 2, 1, 3)
    k = key.permute(0, 2, 1, 3)
    v = value.permute(0, 2, 1, 3)
    return torch.nn.functional.dropout(
        torch.matmul(q, k.transpose(-2, -1)).div(inv_scale_factor).softmax(dim=-1),
        p=dropout_p,
    ).matmul(v)


def _sfdp_replacement_12(query, key, value, inv_scale_factor, dropout_p):
    counters["inductor"]["fuse_attention"] += 1
    return aten.scaled_dot_product_attention(
        query.transpose(1, 2),
        key.transpose(1, 2),
        value.transpose(1, 2),
        attn_mask=None,
        dropout_p=dropout_p,
        is_causal=False,
        scale=1.0 / inv_scale_factor,
    )


def _sfdp_pattern_13(query, key, value, dropout_p):
    attn_weight = torch.bmm(query, key.transpose(1, 2)).softmax(dim=-1)
    attn_weight = torch.nn.functional.dropout(attn_weight, p=dropout_p)
    return torch.bmm(attn_weight, value)


def _sfdp_replacement_13(query, key, value, dropout_p):
    counters["inductor"]["fuse_attention"] += 1
    return aten.scaled_dot_product_attention(
        query.unsqueeze(0),
        key.unsqueeze(0),
        value.unsqueeze(0),
        dropout_p=dropout_p,
        scale=1.0,
    ).squeeze(0)


def _sfdp_pattern_14(query, key, value, attn_mask, inv_scale):
    # for BertLarge
    # Permutations are needed to create clones in graph.
    q = query.permute([0, 2, 1, 3])
    k = key.permute([0, 2, 1, 3])
    v = value.permute([0, 2, 1, 3])
    return (
        (torch.matmul(q, k.transpose(-2, -1)).div(inv_scale) + attn_mask)
        .softmax(dim=-1)
        .matmul(v)
    )


def _sfdp_replacement_14(query, key, value, attn_mask, inv_scale):
    counters["inductor"]["fuse_attention"] += 1
    return aten.scaled_dot_product_attention(
        query.transpose(1, 2),
        key.transpose(1, 2),
        value.transpose(1, 2),
        attn_mask=attn_mask.to(dtype=query.dtype),
        dropout_p=0.0,
        is_causal=False,
        scale=1.0 / inv_scale,
    )


def _sfdp_pattern_15(query, key, value, attn_mask, inv_scale):
    # for DistilBert
    # Permutations are needed to create clones in graph.
    q = query.permute([0, 2, 1, 3])
    k = key.permute([0, 2, 1, 3])
    v = value.permute([0, 2, 1, 3])
    bs = q.size(0)
    k_len = k.size(-2)
    scores = q @ k.transpose(-2, -1)
    scores = scores.div(inv_scale)
    fill_value = torch.full((), -float("inf"), dtype=query.dtype, device=query.device)
    attn_mask = (attn_mask == 0).view((bs, 1, 1, k_len)).expand_as(scores)
    return torch.softmax(scores.masked_fill(attn_mask, fill_value), dim=-1) @ v


def _sfdp_replacement_15(query, key, value, attn_mask, inv_scale):
    counters["inductor"]["fuse_attention"] += 1
    bs = query.size(0)
    n_head = query.size(2)
    q_len = query.size(1)
    k_len = key.size(1)
    # do attn_mask->logical_not() in aten.scaled_dot_product_attention
    attn_mask = (
        (attn_mask == 1).view((bs, 1, 1, k_len)).expand((bs, n_head, q_len, k_len))
    )
    return aten.scaled_dot_product_attention(
        query.transpose(1, 2),
        key.transpose(1, 2),
        value.transpose(1, 2),
        attn_mask=attn_mask.to(dtype=torch.bool),
        dropout_p=0.0,
        is_causal=False,
        scale=1.0 / inv_scale,
    )


def _sfdp_pattern_16(query, key, value, attn_mask, inv_scale, dropout_p):
    # for BertLarge with dropout
    q = query.permute([0, 2, 1, 3])
    k = key.permute([0, 2, 1, 3])
    v = value.permute([0, 2, 1, 3])
    return torch.nn.functional.dropout(
        (torch.matmul(q, k.transpose(-2, -1)).div(inv_scale) + attn_mask).softmax(
            dim=-1
        ),
        dropout_p,
    ).matmul(v)


def _sfdp_replacement_16(query, key, value, attn_mask, inv_scale, dropout_p):
    counters["inductor"]["fuse_attention"] += 1
    return aten.scaled_dot_product_attention(
        query.transpose(1, 2),
        key.transpose(1, 2),
        value.transpose(1, 2),
        attn_mask=attn_mask.to(dtype=query.dtype),
        dropout_p=dropout_p,
        is_causal=False,
        scale=1.0 / inv_scale,
    )


def _sfdp_pattern_17(query, key, value, attn_mask, inv_scale, dropout_p):
    # for DistilBert with dropout
    q = query.permute([0, 2, 1, 3])
    k = key.permute([0, 2, 1, 3])
    v = value.permute([0, 2, 1, 3])
    bs = q.size(0)
    k_len = k.size(-2)
    scores = q @ k.transpose(-2, -1)
    scores = scores.div(inv_scale)
    fill_value = torch.full((), -float("inf"), dtype=query.dtype, device=query.device)
    attn_mask = (attn_mask == 0).view((bs, 1, 1, k_len)).expand_as(scores)
    return (
        torch.nn.functional.dropout(
            torch.softmax(scores.masked_fill(attn_mask, fill_value), dim=-1), dropout_p
        )
        @ v
    )


def _sfdp_replacement_17(query, key, value, attn_mask, inv_scale, dropout_p):
    counters["inductor"]["fuse_attention"] += 1
    bs = query.size(0)
    n_head = query.size(2)
    q_len = query.size(1)
    k_len = key.size(1)
    # do attn_mask->logical_not() in aten.scaled_dot_product_attention
    attn_mask = (
        (attn_mask == 1).view((bs, 1, 1, k_len)).expand((bs, n_head, q_len, k_len))
    )
    return aten.scaled_dot_product_attention(
        query.transpose(1, 2),
        key.transpose(1, 2),
        value.transpose(1, 2),
        attn_mask=attn_mask.to(dtype=torch.bool),
        dropout_p=dropout_p,
        is_causal=False,
        scale=1.0 / inv_scale,
    )


def _sfdp_pattern_18(query, key, value, inv_scale, causal_mask_value, causal_mask):
    # for hf_GPT2 with dropout
    query = query.permute([0, 2, 1, 3])
    key = key.permute([0, 2, 1, 3])
    value = value.permute([0, 2, 1, 3])
    attn_weights = torch.matmul(query, key.permute(0, 1, 3, 2))
    attn_weights = attn_weights.div(inv_scale)
    attn_weights = torch.where(causal_mask, attn_weights, causal_mask_value)
    return (
        (
            torch.nn.functional.dropout(attn_weights.softmax(dim=-1), 0.0)
            .matmul(value)
            .permute([0, 2, 1, 3])
            .contiguous()
        ),
        key,
        value,
    )


def _sfdp_replacement_18(query, key, value, inv_scale, causal_mask_value, causal_mask):
    counters["inductor"]["fuse_attention"] += 1
    query_permuted = query.permute([0, 2, 1, 3])
    key_permuted = key.permute([0, 2, 1, 3])
    value_permuted = value.permute([0, 2, 1, 3])
    attn_bias = torch.zeros(1, 1, query.size(1), query.size(1))
    return (
        torch.ops.mkldnn._graph_sdpa_pattern(
            query_permuted,
            key_permuted.transpose(-1, -2),
            value_permuted,
            None,
            inv_scale,
            attn_bias,  # oneDNN v3.5 will not require this workaround
            causal_mask,
            causal_mask_value,
            False,
            False,
            False,
            False,
            True,
        ),
        key_permuted,
        value_permuted,
    )


def _sfdp_params_check(match):
    assert all(k in match.kwargs for k in ("query", "key", "value"))
    query = match.kwargs["query"].meta["val"]
    key = match.kwargs["key"].meta["val"]
    value = match.kwargs["value"].meta["val"]
    if not (query.dtype == key.dtype == value.dtype) or not (
        query.device == key.device == value.device
    ):
        return False
    add_mask_node = filter_nodes(match.nodes, aten.add.Tensor)
    # Has attn_mask add.
    if len(add_mask_node) > 0:
        attn_mask_node = add_mask_node[0].args[1]
        # attn_mask_node may be a float/int number.
        if not hasattr(attn_mask_node, "meta"):
            return False
        attn_mask = attn_mask_node.meta["val"]  # type: ignore[union-attr]
        # Make sure attn_mask.dtype == query.dtype or attn_mask.dtype == torch.bool
        if (
            not isinstance(attn_mask, torch.Tensor)
            or not (attn_mask.dtype == query.dtype or attn_mask.dtype == torch.bool)
            or query.device != attn_mask.device
        ):
            return False
    return True


# We could add conditions for enabling oneDNN Graph in this check
# But in that case, we wouldn't be able to add replacements for existing patterns
# that already have a replacement defined.
def _onednn_graph_extra_check(match):
    query = match.kwargs["query"].meta["val"]
    if query.dtype not in [torch.float32, torch.bfloat16]:
        return False
    if (query.dtype == torch.bfloat16) and not IS_AVX512_BF16_SUPPORTED:
        return False
    return _sfdp_params_check(match)


def _sfdp_extra_check(scale_factor_op, disable_cuda=False):
    def fn(match):
        scale_factor_node = filter_nodes(match.nodes, scale_factor_op)[0]
        # Note: args[1] of the scale_factor_node is always the scale_factor for the current patterns.
        scale_factor = scale_factor_node.args[1]
        # make sure the scale_factor a float/int. SymInt?
        if not isinstance(scale_factor, (float, int)):
            return False
        if (
            disable_cuda
            and "query" in match.kwargs
            and "cuda" in str(match.kwargs["query"].meta["val"].device)
        ):
            return False
        return _sfdp_params_check(match)

    return fn


def partialize_and_update_signature(func, **kwargs):
    """
    Equivalent to functools.partial but also updates the signature on returned function
    """
    original_sig = inspect.signature(func)
    parameters = original_sig.parameters

    new_parameters = {
        key: value for key, value in parameters.items() if key not in kwargs
    }
    new_sig = inspect.Signature(parameters=list(new_parameters.values()))

    partial_func = functools.partial(func, **kwargs)

    def wrapper(*args, **kwargs):
        return partial_func(*args, **kwargs)

    wrapper.__signature__ = new_sig  # type: ignore[attr-defined]
    wrapper.__name__ = func.__name__

    return wrapper


def _get_sfdp_patterns(enable_onednn_fusions=False, serialization_mode=False):
    from .joint_graph import patterns

    if torch.cuda.is_available():
        # workaround https://github.com/pytorch/pytorch/issues/97894
        device = "cuda"
    else:
        device = "cpu"

    # sizes/values don't actually matter for initial trace
    # once we get a possible match we re-trace with the actual values and verify the match still holds
    g_inp = functools.partial(
        torch.empty, (2, 4, 8, 16), device=device, requires_grad=True
    )
    # attn_mask
    b_inp = functools.partial(torch.empty, (1, 1, 8, 8), device=device)
    m_inp = functools.partial(torch.empty, (2, 1, 1, 4), device=device)
    # inv_scale
    c_inp = functools.partial(torch.tensor, 2.0, device=device)
    # causal_mask
    gpt2_cmask = functools.partial(torch.empty, (1, 1, 4, 4), device=device)
    # workaround https://github.com/pytorch/pytorch/issues/97894
    # 0.113377 is a "magic" value that lets us recover the lost input arg relationship
    d = {"dropout_p": 0.113377}

    # we could also generate all these patterns in 3d.. TODO
    g_3d_inp = functools.partial(
        torch.empty, (1024, 128, 128), device=device, requires_grad=True
    )

    # softmax will generate a dtype conversion on inputs if they are in half,
    # but will not in float, so we generate a pattern for both

    for dtype in [torch.float, torch.half]:
        g = functools.partial(g_inp, dtype=dtype)
        b = functools.partial(b_inp, dtype=dtype)
        m = functools.partial(m_inp, dtype=dtype)
        c = functools.partial(c_inp, dtype=dtype)
        g_3d = functools.partial(g_3d_inp, dtype=dtype)
        gpt2_c = functools.partial(gpt2_cmask, dtype=torch.bool)

        replacement_info = [
            (
                _sfdp_pattern_1,
                _sfdp_replacement_1,
                [g(), g(), g(), c()],
                {},
                _sfdp_extra_check(aten.div.Tensor),
                True,
                True,
                False,
            ),
            (
                _sfdp_pattern_2,
                _sfdp_replacement_2,
                [g(), g(), g(), c()],
                {},
                _sfdp_extra_check(aten.mul.Tensor),
                True,
                True,
                False,
            ),
            (
                _sfdp_pattern_3,
                _sfdp_replacement_3,
                [g(), g(), g(), c()],
                d,
                _sfdp_extra_check(aten.div.Tensor),
                True,
                True,
                False,
            ),
            (
                _sfdp_pattern_4,
                _sfdp_replacement_4,
                [g(), g(), g(), c()],
                d,
                _sfdp_extra_check(aten.mul.Tensor),
                True,
                True,
                False,
            ),
            (
                _sfdp_pattern_5,
                _sfdp_replacement_5,
                [g(), g(), g(), b()],
                {},
                _sfdp_params_check,
                True,
                True,
                False,
            ),
            (
                _sfdp_pattern_6,
                _sfdp_replacement_6,
                [g(), g(), g(), b()],
                d,
                _sfdp_params_check,
                True,
                True,
                False,
            ),
            (
                _sfdp_pattern_7,
                _sfdp_replacement_7,
                [g(), g(), g()],
                d,
                _sfdp_params_check,
                True,
                True,
                False,
            ),
            (
                _sfdp_pattern_8,
                _sfdp_replacement_8,
                [g(), g(), g()],
                {},
                _sfdp_params_check,
                True,
                True,
                False,
            ),
            (
                _sfdp_pattern_9,
                _sfdp_replacement_9,
                [g(), g(), g()],
                d,
                _sfdp_params_check,
                True,
                True,
                False,
            ),
            (
                _sfdp_pattern_10,
                _sfdp_replacement_10,
                [g(), g(), g()],
                {},
                _sfdp_params_check,
                True,
                True,
                False,
            ),
            (
                _sfdp_pattern_11,
                _sfdp_replacement_11,
                [g(), g(), g(), c()],
                {},
                _sfdp_extra_check(aten.div.Tensor),
                True,
                True,
                False,
            ),
            (
                _sfdp_pattern_12,
                _sfdp_replacement_12,
                [g(), g(), g(), c()],
                d,
                _sfdp_extra_check(aten.div.Tensor),
                True,
                True,
                False,
            ),
            (
                _sfdp_pattern_13,
                _sfdp_replacement_13,
                [g_3d(), g_3d(), g_3d()],
                d,
                _sfdp_params_check,
                True,
                True,
                False,
            ),
            (
                _sfdp_pattern_14,
                _sfdp_replacement_14,
                [g(), g(), g(), m(), c()],
                {},
                _sfdp_extra_check(aten.div.Tensor),
                True,
                True,
                False,
            ),
            (
                _sfdp_pattern_15,
                _sfdp_replacement_15,
                [g(), g(), g(), m(), c()],
                {},
                _sfdp_extra_check(aten.div.Tensor),
                True,
                True,
                False,
            ),
            # TODO: Enable CUDA after solving Bert accuracy issue of calling efficient attention
            (
                _sfdp_pattern_16,
                _sfdp_replacement_16,
                [g(), g(), g(), m(), c()],
                d,
                _sfdp_extra_check(aten.div.Tensor, disable_cuda=True),
                True,
                True,
                False,
            ),
            (
                _sfdp_pattern_17,
                _sfdp_replacement_17,
                [g(), g(), g(), m(), c()],
                d,
                _sfdp_extra_check(aten.div.Tensor),
                True,
                True,
                False,
            ),
            (
                _sfdp_pattern_18,
                _sfdp_replacement_18,
                [g(), g(), g(), c(), c(), gpt2_c()],
                {},
                _sfdp_params_check,
                False,
                True,
                True,
            ),
        ]

        for (
            pattern,
            replacement,
            args,
            workaround,
            extra_check,
            register_training,
            register_for_half_dtype,
            onednn_graph_only,
        ) in replacement_info:
            # XXX: when adding a new pattern, re-run `gen_attention_patterns` so the pattern
            # gets serialized to a python file and does not require tracing at runtime.
            assert isinstance(workaround, dict)
            name = pattern.__name__

            if not (dtype == torch.half and not register_for_half_dtype):
                # currently, only inference patterns are supported with oneDNN Graph
                if not enable_onednn_fusions and register_training:
                    training_name = (
                        f"{name}_training"
                        if dtype == torch.float
                        else f"{name}_training_half"
                    )
                    yield training_name, {
                        "search_fn": pattern,
                        "replace_fn": replacement,
                        "example_inputs": args,
                        "trace_fn": joint_fwd_bwd,
                        "pass_dicts": patterns,
                        "extra_check": extra_check,
                        "scalar_workaround": workaround,
                    }

                    if workaround:
                        assert len(workaround) == 1 and "dropout_p" in workaround
                        # functools.partial insufficient because we look at signature downstream
                        pattern = partialize_and_update_signature(
                            pattern, dropout_p=0.0
                        )
                        replacement = partialize_and_update_signature(
                            replacement, dropout_p=0.0
                        )
                        workaround = {}

                if (
                    not serialization_mode
                    and onednn_graph_only
                    and not enable_onednn_fusions
                ):
                    # this pattern is only meant to be used with oneDNN Graph
                    continue

                inference_name = (
                    f"{name}_inference"
                    if dtype == torch.float
                    else f"{name}_inference_half"
                )
                yield inference_name, {
                    "search_fn": pattern,
                    "replace_fn": replacement,
                    "example_inputs": args,
                    "trace_fn": fwd_only,
                    "pass_dicts": patterns,
                    "extra_check": extra_check,
                    "scalar_workaround": workaround,
                }


@functools.lru_cache(None)
def _sfdp_init(enable_onednn_fusions=False):
    from .serialized_patterns.central_index import get_serialized_pattern

    for key, register_replacement_kwargs in _get_sfdp_patterns(
        enable_onednn_fusions=enable_onednn_fusions
    ):
        search_fn_pattern = get_serialized_pattern(key)
        register_replacement(
            **register_replacement_kwargs, search_fn_pattern=search_fn_pattern
        )
