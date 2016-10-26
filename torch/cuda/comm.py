import torch
import torch.cuda.nccl as nccl
from torch._utils import _accumulate

# TODO: sync streams when implemented

def broadcast(tensor, devices):
    "Broadcasts a tensor to a number of GPUs"
    if nccl.is_available([tensor]) and len(set(devices)) == len(devices):
        tensors = [tensor]
        for device in devices[1:]:
            with torch.cuda.device(device):
                tensors.append(type(tensor)(tensor.size()))
        nccl.broadcast(tensors)
        return tuple(tensors)

    # TODO: copy to a pinned buffer first (if copy is from CPU)
    return tuple(tensor.cuda(gpu, async=True) for gpu in devices)


def reduce_add(inputs, destination=None):
    "Reduces tensors from multiple GPUs and returns a result a specified device"
    # TODO: try to find an input on another gpu, copy it,
    # and accumulate into the copy
    input_size = inputs[0].size()
    for i, inp in enumerate(inputs):
        assert inp.is_cuda, "reduce_add expects all inputs to be on GPUs"
        if inp.size() != input_size:
            got = 'x'.join(str(x) for x in inp.size())
            expected = 'x'.join(str(x) for x in input_size)
            raise ValueError("input {} has invalid size: got {}, but expected "
                             "{}".format(i, got, expected))
    if destination is None:
        destination = torch.cuda.current_device()
    with torch.cuda.device(destination):
        result = type(inp)(input_size).zero_()

    if nccl.is_available(inputs) and inputs[0].get_device() == destination:
        outputs = [result] + [t.new(t.size()) for t in inputs[1:]]
        nccl.reduce(inputs, outputs)
        return result

    for inp in inputs:
        input_correct_gpu = inp.cuda(result.get_device())
        result.add_(input_correct_gpu)
    return result


def scatter(tensor, devices, chunk_sizes=None, dim=0):
    "Scatters tensor across multiple GPUs"
    if chunk_sizes is None:
        chunks = tensor.chunk(len(devices), dim)
    else:
        assert sum(chunk_sizes) == tensor.size(dim), "given chunk sizes " \
            "don't sum up to the tensor's size (sum(chunk_sizes) == {}, but " \
            "expected {})".format(sum(chunk_sizes), tensor.size(dim))
        assert min(chunk_sizes) > 0, "got a negative chunk_size"
        chunks = [tensor.narrow(dim, start - size, size)
                  for start, size in zip(_accumulate(chunk_sizes), chunk_sizes)]
    # TODO: copy to a pinned buffer first (if copying from CPU)
    return tuple(chunk.cuda(gpu_id, async=chunk.is_contiguous())
                 for gpu_id, chunk in zip(devices, chunks))


def gather(tensors, dim=0, destination=None):
    """Gathers tensors from multiple GPUs (destination == -1, places the result
       on CPU)
    """
    total_size = 0
    expected_size = list(tensors[0].size())
    for tensor in tensors:
        assert tensor.is_cuda, "gather expects all inputs to be on GPUs"
        expected_size[dim] = tensor.size(dim)
        if list(tensor.size()) != expected_size:
            got = 'x'.join(str(x) for x in tensor.size())
            expected = 'x'.join(str(x) for x in expected_size)
            raise ValueError("gather got an input of invalid size: got {}, "
                             "but expected {}".format(got, expected))
        total_size += tensor.size(dim)
    expected_size[dim] = total_size
    expected_size = torch.Size(expected_size)
    if destination is None:
        destination = torch.cuda.current_device()
    if destination == -1:
        result = getattr(torch, type(tensors[0]).__name__)(expected_size)
    else:
        with torch.cuda.device(destination):
            result = type(tensors[0])(expected_size)

    chunk_start = 0
    # TODO: if copying to CPU, allocate a pinned buffer, do async copies to it,
    # and copy it to regular memory
    for tensor in tensors:
        result.narrow(dim, chunk_start, tensor.size(dim)).copy_(tensor, True)
        chunk_start += tensor.size(dim)
    return result
