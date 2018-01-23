/**
 * Copyright (c) 2016-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <vector>

#include "caffe2/core/context.h"
#include "caffe2/core/operator.h"
#include "caffe2/utils/math.h"

namespace caffe2 {
template <typename F, typename T, class Context>
class NGramFromCategoricalOp : public Operator<Context> {
 public:
  USE_OPERATOR_CONTEXT_FUNCTIONS;

  NGramFromCategoricalOp(const OperatorDef& operator_def, Workspace* ws)
      : Operator<Context>(operator_def, ws),
        col_ids_(OperatorBase::GetRepeatedArgument<int>("col_ids")),
        categorical_limits_(
            OperatorBase::GetRepeatedArgument<int>("categorical_limits")),
        vals_(OperatorBase::GetRepeatedArgument<int>("vals")) {
    col_num_ = col_ids_.size();
    max_col_id_ = *std::max_element(col_ids_.begin(), col_ids_.end());
    CAFFE_ENFORCE_EQ(col_num_, categorical_limits_.size());
    int expected_vals_size = 0;
    for (auto& l : categorical_limits_) {
      CAFFE_ENFORCE_GT(l, 0);
      expected_vals_size += l;
    }
    CAFFE_ENFORCE_EQ(expected_vals_size, vals_.size());
    // compute ngram maps with small end
    for (auto& j : col_ids_) {
      CAFFE_ENFORCE_GE(j, 0);
      ngram_maps_.push_back(std::map<int, int>());
    }
    int base = 1;
    int idx = 0;
    for (int k = 0; k < col_num_; k++) {
      int l = categorical_limits_[k];
      for (int m = 0; m < l; m++) {
        int v = vals_[idx++];
        ngram_maps_[k][v] = m * base;
      }
      base *= l;
    }
  }

  bool RunOnDevice() override {
    auto& floats = Input(0);
    auto N = floats.dim(0);
    auto D = floats.size_from_dim(1);
    const F* floats_data = floats.template data<F>();
    auto* output = Output(0);
    output->Resize(N);
    auto* output_data = output->template mutable_data<T>();
    math::Set<T, Context>(output->size(), 0, output_data, &context_);

    CAFFE_ENFORCE_GT(D, max_col_id_);
    for (int i = 0; i < N; i++) {
      for (int k = 0; k < col_num_; k++) {
        int j = col_ids_[k];
        int v = round(floats_data[i * D + j]);
        // for out-of-vocabulary values, we always treat them the same as the
        // first value specified in vals; if we want to mimic the behavior as
        // sigrid NGram transform, just push front a random/impossible value at
        // each segments of vals
        output_data[i] += ngram_maps_[k].find(v) == ngram_maps_[k].end()
            ? 0
            : ngram_maps_[k][v];
      }
    }
    return true;
  }

 private:
  std::vector<int> col_ids_;
  std::vector<int> categorical_limits_;
  std::vector<int> vals_;
  std::vector<std::map<int, int>> ngram_maps_;
  int col_num_;
  int max_col_id_;
};
} // namespace caffe2
