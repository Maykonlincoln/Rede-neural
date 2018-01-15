from numbers import Number

import torch
from torch.distributions import constraints
from torch.distributions.distribution import Distribution
from torch.distributions.utils import broadcast_all, probs_to_logits, logits_to_probs, lazy_property, _finfo
from torch.nn.functional import binary_cross_entropy_with_logits


class Geometric(Distribution):
    r"""
    Creates a Geometric distribution parameterized by `probs`. It represents the probability that in
    k + 1 Bernoulli trials, the first k trials failed, before seeing a success

    Samples are positive integers [1, inf).

    Example::

        >>> m = Geometric(torch.Tensor([0.3]))
        >>> m.sample()  # underlying Bernoulli has 30% chance 1; 70% chance 0
         2
        [torch.FloatTensor of size 1]

    Args:
        probs (Number, Tensor or Variable): the probabilty of sampling `1`. Must be in range (0, 1]
        logits (Number, Tensor or Variable): the log-odds of sampling `1`.
    """
    params = {'probs': constraints.unit_interval}
    support = constraints.nonnegative_integer

    def __init__(self, probs=None, logits=None):
        if (probs is None) == (logits is None):
            raise ValueError("Either `probs` or `logits` must be specified, but not both.")
        if probs is not None:
            self.probs, = broadcast_all(probs)
            assert self.probs.gt(0).all(), 'All elements of probs must be greater than 0'
        else:
            self.logits, = broadcast_all(logits)
        probs_or_logits = probs if probs is not None else logits
        if isinstance(probs_or_logits, Number):
            batch_shape = torch.Size()
        else:
            batch_shape = probs_or_logits.size()
        super(Geometric, self).__init__(batch_shape)

    @lazy_property
    def logits(self):
        return probs_to_logits(self.probs, is_binary=True)

    @lazy_property
    def probs(self):
        return logits_to_probs(self.logits, is_binary=True)

    def sample(self, sample_shape=torch.Size()):
        shape = self._extended_shape(sample_shape)
        u = self.probs.new(shape).uniform_(_finfo(self.probs).eps, 1)
        return (u.log() / (-self.probs).log1p()).floor()

    def log_prob(self, value):
        self._validate_log_prob_arg(value)
        value, probs = broadcast_all(value, self.probs.clone())
        probs[(probs == 1) & (value == 0)] = 0
        return value * (-probs).log1p() + self.probs.log()

    def entropy(self):
        return binary_cross_entropy_with_logits(self.logits, self.probs, reduce=False) / self.probs
