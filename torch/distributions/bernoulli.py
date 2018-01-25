from numbers import Number

import torch
from torch.autograd import Variable
from torch.distributions import constraints
from torch.distributions.exp_family import ExponentialFamily
from torch.distributions.utils import broadcast_all, probs_to_logits, logits_to_probs, lazy_property
from torch.nn.functional import binary_cross_entropy_with_logits


class Bernoulli(ExponentialFamily):
    r"""
    Creates a Bernoulli distribution parameterized by `probs` or `logits`.

    Samples are binary (0 or 1). They take the value `1` with probability `p`
    and `0` with probability `1 - p`.

    Example::

        >>> m = Bernoulli(torch.Tensor([0.3]))
        >>> m.sample()  # 30% chance 1; 70% chance 0
         0.0
        [torch.FloatTensor of size 1]

    Args:
        probs (Number, Tensor or Variable): the probabilty of sampling `1`
        logits (Number, Tensor or Variable): the log-odds of sampling `1`
    """
    params = {'probs': constraints.unit_interval}
    support = constraints.boolean
    has_enumerate_support = True
    _zero_carrier_measure = True

    def __init__(self, probs=None, logits=None):
        if (probs is None) == (logits is None):
            raise ValueError("Either `probs` or `logits` must be specified, but not both.")
        if probs is not None:
            is_scalar = isinstance(probs, Number)
            self.probs, = broadcast_all(probs)
        else:
            is_scalar = isinstance(logits, Number)
            self.logits, = broadcast_all(logits)
        self._param = self.probs if probs is not None else self.logits
        if is_scalar:
            batch_shape = torch.Size()
        else:
            batch_shape = self._param.size()
        super(Bernoulli, self).__init__(batch_shape)

    def _new(self, *args, **kwargs):
        return self._param.new(*args, **kwargs)

    @lazy_property
    def logits(self):
        return probs_to_logits(self.probs, is_binary=True)

    @lazy_property
    def probs(self):
        return logits_to_probs(self.logits, is_binary=True)

    @property
    def param_shape(self):
        return self._param.size()

    def sample(self, sample_shape=torch.Size()):
        shape = self._extended_shape(sample_shape)
        return torch.bernoulli(self.probs.expand(shape))

    def log_prob(self, value):
        self._validate_log_prob_arg(value)
        logits, value = broadcast_all(self.logits, value)
        return -binary_cross_entropy_with_logits(logits, value, reduce=False)

    def entropy(self):
        return binary_cross_entropy_with_logits(self.logits, self.probs, reduce=False)

    def enumerate_support(self):
        values = self._new((2,))
        torch.arange(2, out=values.data if isinstance(values, Variable) else values)
        values = values.view((-1,) + (1,) * len(self._batch_shape))
        values = values.expand((-1,) + self._batch_shape)
        return values

    def natural_params(self):
        return self._natural_params

    @lazy_property
    def _natural_params(self):
        if isinstance(self.probs, Variable):
            V1 = Variable(torch.log(self.probs.data / (1 - self.probs.data)), requires_grad=True)
        else:
            V1 = Variable(torch.log(self.probs / (1 - self.probs)), requires_grad=True)
        return (V1, )

    def log_normalizer(self):
        x, = self._natural_params
        return torch.log(1 + torch.exp(x))
