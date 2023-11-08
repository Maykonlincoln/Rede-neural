# flake8: noqa: F401
r"""Quantized Modules.

This file is in the process of migration to `torch/ao/nn/quantized`, and
is kept here for compatibility while the migration process is ongoing.
If you are adding a new entry/functionality, please, add it to the
appropriate file under the `torch/ao/nn/quantized/modules`,
while adding an import statement here.
"""

__all__ = ['Conv1d', 'Conv2d', 'Conv3d', 'ConvTranspose1d', 'ConvTranspose2d', 'ConvTranspose3d']

from torch.ao.nn.quantized.modules.conv import _reverse_repeat_padding

from torch.ao.nn.quantized.modules.conv import Conv1d
from torch.ao.nn.quantized.modules.conv import Conv2d
from torch.ao.nn.quantized.modules.conv import Conv3d

from torch.ao.nn.quantized.modules.conv import ConvTranspose1d
from torch.ao.nn.quantized.modules.conv import ConvTranspose2d
from torch.ao.nn.quantized.modules.conv import ConvTranspose3d
