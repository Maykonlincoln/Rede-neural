#include "caffe2/core/tensor.h"

#include "caffe2/core/flags.h"

CAFFE2_DEFINE_bool(
    caffe2_keep_on_shrink,
    true,
    "If set, keeps memory when a tensor is shrinking its size.");

namespace caffe2 {
// declaring it here instead of context.cc because tensor.h includes context.h
CAFFE_KNOWN_TYPE(Tensor<CPUContext>);

TensorPrinter::TensorPrinter(
    const std::string& tensor_name,
    const std::string& file_name,
    int limit)
    : to_file_(!file_name.empty()),
      limit_(limit ? limit : k_limit_default_),
      tensor_name_(tensor_name) {
  if (to_file_) {
    // We will output to file instead of printing on screen.
    // We will write each individual tensor to its individual file.
    log_file_.reset(new std::ofstream(
        file_name, std::ofstream::out | std::ofstream::trunc));
    CAFFE_ENFORCE(
        log_file_->good(),
        "Failed to open TensorPrinter file ",
        file_name,
        ". rdstate() = ",
        log_file_->rdstate());
  }
}

TensorPrinter::~TensorPrinter() {
  if (log_file_.get()) {
    log_file_->close();
  }
}

std::string TensorPrinter::MetaStr(const Tensor<CPUContext>& tensor) {
  std::stringstream meta_stream;
  meta_stream << "Tensor " << tensor_name_ << " of type "
              << tensor.meta().name() << ". Dims: (";
  for (const auto dim : tensor.dims()) {
    meta_stream << dim << ",";
  }
  meta_stream << "): ";
  return meta_stream.str();
}

static CaffeMap<CaffeTypeId, ShapeCall> shape_call_registry_ {
  {TypeMeta::Id<Tensor<CPUContext>>(), GetTensorShape<Tensor<CPUContext>>}
};

ShapeCall GetShapeCallFunction(CaffeTypeId id) {
  auto f = shape_call_registry_.find(id);
  if (f == shape_call_registry_.end()) {
    return nullptr;
  }
  return f->second;
}

void RegisterShapeCallFunction(CaffeTypeId id, ShapeCall c) {
  shape_call_registry_[id] = c;
}

} // namespace caffe2
