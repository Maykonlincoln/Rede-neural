#include "ATen/ATen.h"

#include <ATen/Error.h>

#include <algorithm>
#include <vector>

namespace at {
namespace native {

// wrap negative dims
static inline void wrap_dims(std::vector<int64_t>& v, int64_t n) {
  for (int64_t i = 0; i < v.size(); i++) {
    if (v[i] < 0) {
      v[i] = (n + (v[i] % n)) % n;
    }
  }
}

static inline void flip_check_errors(int64_t total_dims, int64_t flip_dims_size, IntList dims) {
  // check if number of axis in dim is valid
  AT_CHECK(flip_dims_size > 0 && flip_dims_size <= total_dims,
    "flip dims size out of range, got flip dims size=", flip_dims_size);

  auto flip_dims_v = std::vector<int64_t>(dims);

  // check if dims axis within range
  auto min_max_d = std::minmax_element(flip_dims_v.begin(), flip_dims_v.end());

  AT_CHECK(*min_max_d.first < total_dims && *min_max_d.first >= -total_dims,
    "The min flip dims out of range, got min flip dims=", *min_max_d.first);

  AT_CHECK(*min_max_d.second < total_dims && *min_max_d.second >= -total_dims,
    "The max flip dims out of range, got max flip dims=", *min_max_d.second);

  // check duplicates in dims
  wrap_dims(flip_dims_v, total_dims);
  flip_dims_v.erase(std::unique(flip_dims_v.begin(), flip_dims_v.end()), flip_dims_v.end());
  AT_CHECK((int64_t)flip_dims_v.size() == flip_dims_size,
    "dims has duplicates, original flip dims size=", flip_dims_size,
    ", but unique flip dims size=", flip_dims_v.size());
}

}}  // namespace at::native
