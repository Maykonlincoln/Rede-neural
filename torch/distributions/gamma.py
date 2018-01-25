from numbers import Number

import torch
from torch.autograd import Function, Variable
from torch.autograd.function import once_differentiable
from torch.distributions import constraints
from torch.distributions.exp_family import ExponentialFamily
from torch.distributions.utils import _finfo, broadcast_all, lazy_property


def _standard_gamma(concentration):
    if not isinstance(concentration, Variable):
        return torch._C._standard_gamma(concentration)
    return concentration._standard_gamma()


class Gamma(ExponentialFamily):
    r"""
    Creates a Gamma distribution parameterized by shape `concentration` and `rate`.

    Example::

        >>> m = Gamma(torch.Tensor([1.0]), torch.Tensor([1.0]))
        >>> m.sample()  # Gamma distributed with concentration=1 and rate=1
         0.1046
        [torch.FloatTensor of size 1]

    Args:
        concentration (float or Tensor or Variable): shape parameter of the distribution
            (often referred to as alpha)
        rate (float or Tensor or Variable): rate = 1 / scale of the distribution
            (often referred to as beta)
    """
    params = {'concentration': constraints.positive, 'rate': constraints.positive}
    support = constraints.positive
    has_rsample = True
    _zero_carrier_measure = True

    def __init__(self, concentration, rate):
        self.concentration, self.rate = broadcast_all(concentration, rate)
        if isinstance(concentration, Number) and isinstance(rate, Number):
            batch_shape = torch.Size()
        else:
            batch_shape = self.concentration.size()
        super(Gamma, self).__init__(batch_shape)

    def rsample(self, sample_shape=torch.Size()):
        shape = self._extended_shape(sample_shape)
        value = _standard_gamma(self.concentration.expand(shape)) / self.rate.expand(shape)
        data = value.data if isinstance(value, Variable) else value
        data.clamp_(min=_finfo(value).tiny)  # do not record in autograd graph
        return value

    def log_prob(self, value):
        self._validate_log_prob_arg(value)
        return (self.concentration * torch.log(self.rate) +
                (self.concentration - 1) * torch.log(value) -
                self.rate * value - torch.lgamma(self.concentration))

    def entropy(self):
        return (self.concentration - torch.log(self.rate) + torch.lgamma(self.concentration) +
                (1.0 - self.concentration) * torch.digamma(self.concentration))

    def natural_params(self):
        return self._natural_params

    @lazy_property
    def _natural_params(self):
        if isinstance(self.concentration, Variable):
            V1 = Variable(self.concentration.data - 1, requires_grad=True)
            V2 = Variable(-self.rate.data, requires_grad=True)
        else:
            V1 = Variable(self.concentration - 1, requires_grad=True)
            V2 = Variable(-self.rate, requires_grad=True)
        return (V1, V2)

    def log_normalizer(self):
        x, y = self._natural_params
        t1 = x + 1
        return torch.lgamma(t1) + t1 * torch.log(-y.reciprocal())
