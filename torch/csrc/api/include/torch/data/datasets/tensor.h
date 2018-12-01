#pragma once

#include <torch/data/datasets/base.h>
#include <torch/data/example.h>
#include <torch/types.h>

#include <cstddef>
#include <vector>

namespace torch {
namespace data {
namespace datasets {

/// A dataset of tensors.
/// Stores a single tensor internally, which is then indexed inside `get()`.
struct TensorDataset : public Dataset<TensorDataset, TensorExample> {
  /// Creates a `TensorDataset` from a vector of tensors.
  explicit TensorDataset(const std::vector<Tensor>& tensors)
      : TensorDataset(torch::stack(tensors)) {}

  explicit TensorDataset(torch::Tensor tensor) : tensor(std::move(tensor)) {}

  /// Returns a single `TensorExample`.
  optional<TensorExample> get(size_t index) override {
    return tensor[index];
  }

  /// Returns the number of tensors in the dataset.
  optional<size_t> size() const override {
    return tensor.size(0);
  }

  Tensor tensor;
};

} // namespace datasets
} // namespace data
} // namespace torch
