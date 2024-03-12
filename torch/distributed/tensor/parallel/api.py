# Copyright (c) Meta Platforms, Inc. and affiliates
from typing import Dict, Union

import torch
import torch.distributed._tensor.random as random
import torch.nn as nn
from torch.distributed._tensor import (
    DeviceMesh,
)
from torch.distributed._tensor.random import (
    is_rng_supported_mesh,
    TensorParallelRNGTracker,
)
from torch.distributed.tensor.parallel._utils import _validate_tp_mesh_dim
from torch.distributed.tensor.parallel.style import (
    ParallelStyle,
)


__all__ = [
    "parallelize_module",
]


def parallelize_module(  # type: ignore[return]
    module: nn.Module,
    device_mesh: DeviceMesh,
    parallelize_plan: Union[ParallelStyle, Dict[str, ParallelStyle]],
) -> nn.Module:
    """
    Apply Tensor Parallelism in PyTorch by parallelizing modules or sub-modules based on a user-specified plan.

    We parallelize module or sub_modules based on a parallelize_plan. The parallelize_plan contains
    :class:`ParallelStyle`, which indicates how user wants the module or sub_module
    to be parallelized.

    User can also specify different parallel style per module fully qualified name (FQN).

    Note that ``parallelize_module`` only accepts a 1-D :class:`DeviceMesh`, if you have a 2-D or N-D :class:`DeviceMesh`,
    slice the DeviceMesh to a 1-D sub DeviceMesh first then pass to this API(i.e. ``device_mesh[\"tp\"]``)

    Args:
        module (:class:`nn.Module`):
            Module to be parallelized.
        device_mesh (:class:`DeviceMesh`):
            Object which describes the mesh topology
            of devices for the DTensor.
        parallelize_plan (Union[:class:`ParallelStyle`, Dict[str, :class:`ParallelStyle`]]):
            The plan used to parallelize the module. It can be either a
            :class:`ParallelStyle` object which contains how
            we prepare input/output for Tensor Parallelism or it can be a
            dict of module FQN and its corresponding :class:`ParallelStyle` object.
    Return:
        A :class:`nn.Module` object parallelized.

    Example::
        >>> # xdoctest: +SKIP("distributed")
        >>> from torch.distributed.tensor.parallel import parallelize_module, ColwiseParallel
        >>> from torch.distributed.device_mesh import init_device_mesh
        >>>
        >>> # Define the module.
        >>> m = Model(...)
        >>> tp_mesh = init_device_mesh("cuda", (8,))
        >>> m = parallelize_module(m, tp_mesh, {"w1": ColwiseParallel(), "w2": RowwiseParallel()})
        >>>

    .. note:: For complex module architecture like Attention, MLP layers, we recommend composing
        different ParallelStyles together (i.e. ``ColwiseParallel`` and ``RowwiseParallel``) and pass
        as a parallelize_plan, to achieves the desired sharding computation.
    """
    torch._C._log_api_usage_once("torch.distributed.tensor.parallel.parallelize_module")

    _validate_tp_mesh_dim(device_mesh)

    # instantiate a TP RNG state tracker if it's not there
    if is_rng_supported_mesh(device_mesh) and not isinstance(
        random._rng_tracker, TensorParallelRNGTracker
    ):
        random._rng_tracker = TensorParallelRNGTracker(device_mesh.device_type)
        # TODO: we should allow user to pass in the default seed from a config
        random._rng_tracker._manual_seed(device_mesh, base_seed=1234)
        # By default we execute random ops in non-tensor-parallel region. If users want
        # to execute in tensor-parallel region, they can manually set this field to True
        # after parallelizing the model.
        random._rng_tracker.distribute_region_enabled = False

    if isinstance(parallelize_plan, ParallelStyle):
        return parallelize_plan._apply(module, device_mesh)
    elif isinstance(parallelize_plan, dict):
        for module_path, parallelize_style in parallelize_plan.items():
            sub_module = module.get_submodule(module_path)
            parent_module = module
            if "." in module_path:
                parent_module_path = ".".join(module_path.split(".")[:-1])
                parent_module = module.get_submodule(parent_module_path)
                module_path = module_path.split(".")[-1]
            parent_module.register_module(  # type: ignore[call-arg] # pyre-ignore[20]
                module_path,
                parallelize_module(  # type: ignore[arg-type]
                    sub_module, device_mesh, parallelize_style  # type: ignore[arg-type] # pyre-ignore[6]
                ),
            )
        return module
    else:
        raise RuntimeError(  # pyre-ignore[7]
            "Expect Union[ParallelStyle, Dict[str, ParallelStyle]] for"
            f" parallelize_plan, {type(parallelize_plan)} found!"
        )
