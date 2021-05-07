import functools
from typing import Any, Dict, NamedTuple, Optional, Set, Tuple, List, Callable, Union
import torch
from torch.fx.node import map_aggregate
import torch.utils._pytree as pytree
from functorch._C import hasPythonKey, addPythonKey, removePythonKey
from torch.fx import Tracer, GraphModule
import torch.fx as fx
from .nnc_compile import nnc_compile

class PythonTensor(object):
    def __init__(self, out, proxy):
        if isinstance(out, torch.Tensor):
            self.value = torch.clone(out)
        else:
            self.value = torch.empty(out)
        self.proxy = proxy

    def __repr__(self):
        return f"PythonTensor({tuple(self.value.shape)})"

    def tensor(self):
        return self.value

    def __torch_function__(self, func, types, args=(), kwargs={}):
        namespace, func_name = func.split("::")
        func = getattr(getattr(torch.ops, namespace), func_name)
        outs = kwargs['val']
        rets = []
        proxy_args = map_aggregate(args, lambda i: i.proxy if isinstance(i, PythonTensor) else i)
        out_proxy = func(*proxy_args)
        if len(outs) == 1 and isinstance(outs[0], torch.Tensor):
            return [PythonTensor(outs[0], out_proxy)]
        for idx, out in enumerate(outs):
            if isinstance(out, torch.Tensor):
                rets.append(PythonTensor(out, out_proxy[idx]))
            else:
                rets.append(out)
        return rets

class PythonKeyTracer(Tracer):
    def __init__(self):
        super().__init__()


    def call_module(self, m: torch.nn.Module, forward: Callable[..., Any], args : Tuple[Any, ...], kwargs : Dict[str, Any]) -> Any:
        """
        Method that specifies the behavior of this ``Tracer`` when it encounters
        a call to an ``nn.Module`` instance.

        By default, the behavior is to check if the called module is a leaf module
        via ``is_leaf_module``. If it is, emit a ``call_module`` node referring to
        ``m`` in the ``Graph``. Otherwise, call the ``Module`` normally, tracing through
        the operations in its ``forward`` function.

        This method can be overridden to--for example--create nested traced
        GraphModules, or any other behavior you would want while tracing across
        ``Module`` boundaries.

        Args:

            m (Module): The module for which a call is being emitted
            forward (Callable): The forward() method of the ``Module`` to be invoked
            args (Tuple): args of the module callsite
            kwargs (Dict): kwargs of the module callsite

        Return:

            The return value from the Module call. In the case that a ``call_module``
            node was emitted, this is a ``Proxy`` value. Otherwise, it is whatever
            value was returned from the ``Module`` invocation.
        """
        return forward(*args, **kwargs)

    def module_getattr(self, attr, attr_val):
        if isinstance(attr_val, torch.nn.Parameter):
            for n, p in self.root.named_parameters():
                if attr_val is p:
                    if n not in self.parameter_proxy_cache:
                        proxy = self.create_proxy('get_attr', n, (), {})
                        self.parameter_proxy_cache[n] = addPythonKey(PythonTensor(attr_val.shape, proxy))
                    return self.parameter_proxy_cache[n]
        return attr_val

def pythonkey_trace(root : Union[torch.nn.Module, Callable], concrete_args: Optional[Dict[str, Any]] = None) -> GraphModule:
    tracer = PythonKeyTracer()
    graph = tracer.trace(root, concrete_args)
    name = root.__class__.__name__ if isinstance(root, torch.nn.Module) else root.__name__
    return GraphModule(tracer.root, graph, name)


class WrapModule(torch.nn.Module):
    def __init__(self, mod, inps):
        super().__init__()
        self.mod = mod
        self.inps = inps
        @functools.wraps(mod.forward)
        def forward_wrapped(self, *args):
            new_args = []
            for inp, arg in zip(inps, args):
                if isinstance(inp, torch.Tensor):
                    new_arg = addPythonKey(PythonTensor(inp.shape, arg))
                else:
                    new_arg = inp
                new_args.append(new_arg)
            out = self.mod(*new_args)

            flat_outs, out_spec = pytree.tree_flatten(out)
            for idx in range(len(flat_outs)):
                if hasPythonKey(flat_outs[idx]):
                    flat_outs[idx] = removePythonKey(flat_outs[idx]).proxy
            return pytree.tree_unflatten(flat_outs, out_spec)

        type(self).forward = forward_wrapped

def wrap_key(f, inps):
    flat_inps, inp_spec = pytree.tree_flatten(inps)
    @functools.wraps(f)
    def wrapped(*args):
        flat_args, args_spec = pytree.tree_flatten(args)
        assert(len(flat_args) == len(flat_inps))
        for idx, arg in enumerate(flat_args):
            if isinstance(flat_inps[idx], torch.Tensor):
                flat_args[idx] = addPythonKey(PythonTensor(flat_inps[idx], arg))
            else:
                flat_args[idx] = flat_inps[idx]

        tree_args = pytree.tree_unflatten(flat_args, args_spec)
        out = f(*tree_args)

        flat_outs, out_spec = pytree.tree_flatten(out)
        for idx in range(len(flat_outs)):
            if isinstance(flat_outs[idx], torch.Tensor) and hasPythonKey(flat_outs[idx]):
                flat_outs[idx] = removePythonKey(flat_outs[idx]).proxy
        return pytree.tree_unflatten(flat_outs, out_spec)

    return wrapped

def make_fx(f):
    @functools.wraps(f)
    def wrapped(*args):
        phs = pytree.tree_map(lambda x: fx.PH, args)
        t = pythonkey_trace(wrap_key(f, args), concrete_args=tuple(phs))
        return t

    return wrapped


def nnc_jit(f):
    cached = None
    @functools.wraps(f)
    def compiled(*args):
        nonlocal cached
        if cached is not None:
            return cached(*args)
        fx_model = make_fx(f)(*args)
        fx_model.graph.lint()
        compiled_f = nnc_compile(fx_model, args)
        cached = compiled_f
        return cached(*args)
    return compiled

def make_nnc(f):
    @functools.wraps(f)
    def wrapped(*args):
        fx_model = make_fx(f)(*args)
        fx_model.graph.lint()
        compiled_f = nnc_compile(fx_model, args, get_loopnest=True)
        return compiled_f

    return wrapped