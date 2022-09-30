# DO NOT EDIT! This file was generated by jschema_to_python version 0.0.1.dev29,
# with extension for dataclasses and type annotation.

from __future__ import annotations

import dataclasses
from typing import Any, List, Optional

from torch.onnx._internal.diagnostics.infra.sarif import (
    _message,
    _property_bag,
    _thread_flow_location,
)


@dataclasses.dataclass
class ThreadFlow(object):
    """Describes a sequence of code locations that specify a path through a single thread of execution such as an operating system or fiber."""

    locations: List[_thread_flow_location.ThreadFlowLocation] = dataclasses.field(
        metadata={"schema_property_name": "locations"}
    )
    id: Optional[str] = dataclasses.field(
        default=None, metadata={"schema_property_name": "id"}
    )
    immutable_state: Any = dataclasses.field(
        default=None, metadata={"schema_property_name": "immutableState"}
    )
    initial_state: Any = dataclasses.field(
        default=None, metadata={"schema_property_name": "initialState"}
    )
    message: Optional[_message.Message] = dataclasses.field(
        default=None, metadata={"schema_property_name": "message"}
    )
    properties: Optional[_property_bag.PropertyBag] = dataclasses.field(
        default=None, metadata={"schema_property_name": "properties"}
    )


# flake8: noqa
