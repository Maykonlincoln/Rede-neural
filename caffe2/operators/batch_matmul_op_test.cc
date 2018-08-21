#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "caffe2/operators/batch_matmul_op.h"

namespace caffe2 {
namespace {

class BatchMatMulOpTest : public testing::Test {
 protected:
  void SetUp() override {
    cpu_context_ = make_unique<CPUContext>(option_);
    def_.set_name("test");
    def_.set_type("BatchMatMul");
    def_.add_input("A");
    def_.add_input("B");
    def_.add_output("Y");
  }

  void AddConstInput(
      const std::vector<TIndex>& dims,
      const float value,
      const string& name) {
    Blob* blob = ws_.CreateBlob(name);
    auto* tensor = blob->GetMutableTensor(CPU);
    tensor->Resize(dims);
    math::Set<float, CPUContext>(
        tensor->size(),
        value,
        tensor->template mutable_data<float>(),
        cpu_context_.get());
  }

  void VerifyOutput(const std::vector<TIndex>& dims, const float value) const {
    const Blob* Y_blob = ws_.GetBlob("Y");
    ASSERT_NE(nullptr, Y_blob);
    const auto& Y = Y_blob->Get<TensorCPU>();
    const auto& Y_dims = Y.dims();
    ASSERT_EQ(dims.size(), Y_dims.size());
    for (std::size_t i = 0; i < dims.size(); ++i) {
      ASSERT_EQ(dims[i], Y_dims[i]);
    }
    for (int i = 0; i < Y.size(); ++i) {
      EXPECT_FLOAT_EQ(value, Y.data<float>()[i]);
    }
  }

  DeviceOption option_;
  std::unique_ptr<CPUContext> cpu_context_;
  Workspace ws_;
  OperatorDef def_;
};

TEST_F(BatchMatMulOpTest, BatchMatMulOpNormalTest) {
  AddConstInput(std::vector<TIndex>{3, 5, 10}, 1.0f, "A");
  AddConstInput(std::vector<TIndex>{3, 10, 6}, 1.0f, "B");
  std::unique_ptr<OperatorBase> op(CreateOperator(def_, &ws_));
  ASSERT_NE(nullptr, op);
  ASSERT_TRUE(op->Run());
  VerifyOutput(std::vector<TIndex>{3, 5, 6}, 10.0f);
}

TEST_F(BatchMatMulOpTest, BatchMatMulOpBroadcastTest) {
  auto* arg = def_.add_arg();
  arg->set_name("broadcast");
  arg->set_i(1);
  AddConstInput(std::vector<TIndex>{3, 5, 10}, 1.0f, "A");
  AddConstInput(std::vector<TIndex>{2, 3, 10, 6}, 1.0f, "B");
  std::unique_ptr<OperatorBase> op(CreateOperator(def_, &ws_));
  ASSERT_NE(nullptr, op);
  ASSERT_TRUE(op->Run());
  VerifyOutput(std::vector<TIndex>{2, 3, 5, 6}, 10.0f);
}

} // namespace
} // namespace caffe2
