#include <gtest/gtest.h>
#include "caffe2/core/common.h"
#include "caffe2/core/logging.h"
#include "caffe2/opt/bound_shape_inferencer.h"
#include "caffe2/utils/proto_utils.h"

using namespace caffe2;
namespace {

ShapeInfo makeTensorInfo(
    const std::vector<TensorBoundShape::DimType>& t,
    const std::vector<int64_t>& dims,
    TensorProto::DataType dtype = TensorProto_DataType_FLOAT,
    bool quantized = false) {
  ShapeInfo info;
  info.setDimType(t);
  TensorShape& shape = info.shape;
  for (const auto d : dims) {
    shape.add_dims(d);
  }
  shape.set_data_type(dtype);
  if (quantized) {
    info.is_quantized = true;
    info.q_info.scale.clear();
    info.q_info.scale.push_back(1);
    info.q_info.offset.clear();
    info.q_info.offset.push_back(0);
    info.q_info.axis = 1;
  }
  return info;
}

void verifyShapeInfo(
    const ShapeInfoMap& info,
    const std::string& name,
    const std::vector<TensorBoundShape::DimType>& t,
    const std::vector<int64_t>& dims,
    TensorProto::DataType dtype = TensorProto_DataType_FLOAT,
    bool quantized = false) {
  LOG(INFO) << "Checking " << name;
  const auto it = info.find(name);
  ASSERT_TRUE(it != info.end());
  const auto& shape_info = it->second;
  EXPECT_EQ(shape_info.getDimType(), t);
  const auto& shape = shape_info.shape;
  ASSERT_EQ(shape.dims_size(), dims.size());
  for (int i = 0; i < dims.size(); ++i) {
    EXPECT_EQ(shape.dims(i), dims[i]);
  }
  EXPECT_EQ(shape.data_type(), dtype);
  EXPECT_EQ(shape_info.is_quantized, quantized);
}

} // namespace

TEST(BoundShapeInference, SparseLengthsSum) {
  NetDef net;
  net.add_op()->CopyFrom(CreateOperatorDef(
      "SparseLengthsSum", "", {"Weights", "Data", "Lengths"}, {"Out"}, {}));
  ShapeInfoMap shape_map;
  shape_map.emplace(
      "Weights",
      makeTensorInfo(
          {TensorBoundShape_DimType_CONSTANT,
           TensorBoundShape_DimType_CONSTANT},
          {1000, 16}));
  BoundShapeSpec spec(20, 1000);
  BoundShapeInferencer eng(spec);
  eng.InferBoundShapeAndType(net, shape_map, nullptr);
  const auto& out_shape = eng.shape_info();
  verifyShapeInfo(
      out_shape,
      "Weights",
      {TensorBoundShape_DimType_CONSTANT, TensorBoundShape_DimType_CONSTANT},
      {1000, 16});
  verifyShapeInfo(
      out_shape,
      "Data",
      {TensorBoundShape_DimType_FEATURE_MAX_DEFAULT},
      {spec.max_seq_size},
      TensorProto_DataType_INT64);
  verifyShapeInfo(
      out_shape,
      "Lengths",
      {TensorBoundShape_DimType_BATCH},
      {spec.max_batch_size},
      TensorProto_DataType_INT32);
  verifyShapeInfo(
      out_shape,
      "Out",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 16});
}

TEST(BoundShapeInference, SparseLengthsSumFused8BitRowwise) {
  NetDef net;
  net.add_op()->CopyFrom(CreateOperatorDef(
      "SparseLengthsSumFused8BitRowwise",
      "",
      {"Weights", "Data", "Lengths"},
      {"Out"},
      {}));
  ShapeInfoMap shape_map;
  shape_map.emplace(
      "Weights",
      makeTensorInfo(
          {TensorBoundShape_DimType_CONSTANT,
           TensorBoundShape_DimType_CONSTANT},
          {1000, 58},
          TensorProto_DataType_INT8));
  BoundShapeSpec spec(20, 1000);
  BoundShapeInferencer eng(spec);
  eng.InferBoundShapeAndType(net, shape_map, nullptr);
  const auto& out_shape = eng.shape_info();
  verifyShapeInfo(
      out_shape,
      "Weights",
      {TensorBoundShape_DimType_CONSTANT, TensorBoundShape_DimType_CONSTANT},
      {1000, 58},
      TensorProto_DataType_INT8);
  verifyShapeInfo(
      out_shape,
      "Data",
      {TensorBoundShape_DimType_FEATURE_MAX_DEFAULT},
      {spec.max_seq_size},
      TensorProto_DataType_INT64);
  verifyShapeInfo(
      out_shape,
      "Lengths",
      {TensorBoundShape_DimType_BATCH},
      {spec.max_batch_size},
      TensorProto_DataType_INT32);
  verifyShapeInfo(
      out_shape,
      "Out",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 50});
}

TEST(BoundShapeInference, SparseLengthsSumFused4BitRowwise) {
  NetDef net;
  net.add_op()->CopyFrom(CreateOperatorDef(
      "SparseLengthsSumFused4BitRowwise",
      "",
      {"Weights", "Data", "Lengths"},
      {"Out"},
      {}));
  ShapeInfoMap shape_map;
  shape_map.emplace(
      "Weights",
      makeTensorInfo(
          {TensorBoundShape_DimType_CONSTANT,
           TensorBoundShape_DimType_CONSTANT},
          {1000, 54},
          TensorProto_DataType_INT8));
  BoundShapeSpec spec(20, 1000);
  BoundShapeInferencer eng(spec);
  eng.InferBoundShapeAndType(net, shape_map, nullptr);
  const auto& out_shape = eng.shape_info();
  verifyShapeInfo(
      out_shape,
      "Weights",
      {TensorBoundShape_DimType_CONSTANT, TensorBoundShape_DimType_CONSTANT},
      {1000, 54},
      TensorProto_DataType_INT8);
  verifyShapeInfo(
      out_shape,
      "Data",
      {TensorBoundShape_DimType_FEATURE_MAX_DEFAULT},
      {spec.max_seq_size},
      TensorProto_DataType_INT64);
  verifyShapeInfo(
      out_shape,
      "Lengths",
      {TensorBoundShape_DimType_BATCH},
      {spec.max_batch_size},
      TensorProto_DataType_INT32);
  verifyShapeInfo(
      out_shape,
      "Out",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 100});
}

TEST(BoundShapeInference, LengthsRangeFill) {
  NetDef net;
  net.add_op()->CopyFrom(
      CreateOperatorDef("LengthsRangeFill", "", {"X"}, {"Y"}, {}));
  net.add_op()->CopyFrom(CreateOperatorDef("Copy", "", {"Y"}, {"Z"}, {}));
  ShapeInfoMap shape_map;
  BoundShapeSpec spec(20, 1000);
  BoundShapeInferencer eng(spec);
  eng.InferBoundShapeAndType(net, shape_map, nullptr);
  const auto& out_shape = eng.shape_info();
  verifyShapeInfo(
      out_shape,
      "X",
      {TensorBoundShape_DimType_BATCH},
      {spec.max_batch_size},
      TensorProto_DataType_INT32);
  verifyShapeInfo(
      out_shape,
      "Y",
      {TensorBoundShape_DimType_FEATURE_MAX_DEFAULT},
      {spec.max_seq_size},
      TensorProto_DataType_INT32);
  verifyShapeInfo(
      out_shape,
      "Z",
      {TensorBoundShape_DimType_FEATURE_MAX_DEFAULT},
      {spec.max_seq_size},
      TensorProto_DataType_INT32);
}

TEST(BoundShapeInference, Gather) {
  NetDef net;
  net.add_op()->CopyFrom(
      CreateOperatorDef("Gather", "", {"Data", "Indices"}, {"Out"}, {}));
  ShapeInfoMap shape_map;
  shape_map.emplace(
      "Data",
      makeTensorInfo(
          {TensorBoundShape_DimType_CONSTANT,
           TensorBoundShape_DimType_CONSTANT},
          {30, 20},
          TensorProto_DataType_INT64));
  BoundShapeSpec spec(20, 1000);
  BoundShapeInferencer eng(spec);
  eng.InferBoundShapeAndType(net, shape_map, nullptr);
  const auto& out_shape = eng.shape_info();
  verifyShapeInfo(
      out_shape,
      "Out",
      {TensorBoundShape_DimType_BATCH,
       TensorBoundShape_DimType_FEATURE_MAX_DEFAULT,
       TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, spec.max_seq_size, 20},
      TensorProto_DataType_INT64);
}

TEST(BoundShapeInference, Reshape) {
  NetDef net;
  std::vector<int> new_shape{-1, 8};
  std::vector<int> new_shape2{2, 8};
  net.add_op()->CopyFrom(
      CreateOperatorDef("FC", "", {"X0", "W0", "B0"}, {"X1"}, {}));
  net.add_op()->CopyFrom(CreateOperatorDef(
      "Reshape",
      "",
      {"X1"},
      {"Y1", "old_shape"},
      {MakeArgument<std::vector<int>>("shape", new_shape)}));

  // Cannot infer shape for this one because input/output shape doesn't match
  net.add_op()->CopyFrom(CreateOperatorDef(
      "Reshape",
      "",
      {"X1"},
      {"Y2", "old_shape2"},
      {MakeArgument<std::vector<int>>("shape", new_shape2)}));
  ShapeInfoMap shape_map;
  shape_map.emplace(
      "W0",
      makeTensorInfo(
          {TensorBoundShape_DimType_CONSTANT,
           TensorBoundShape_DimType_CONSTANT},
          {16, 1024}));
  shape_map.emplace(
      "B0", makeTensorInfo({TensorBoundShape_DimType_CONSTANT}, {16}));
  BoundShapeSpec spec(20, 1000);
  BoundShapeInferencer eng(spec);
  eng.InferBoundShapeAndType(net, shape_map, nullptr);
  const auto& out_shape = eng.shape_info();
  verifyShapeInfo(
      out_shape,
      "X0",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 1024});
  verifyShapeInfo(
      out_shape,
      "X1",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 16});
  verifyShapeInfo(
      out_shape,
      "Y1",
      {TensorBoundShape_DimType_BATCH,
       TensorBoundShape_DimType_CONSTANT}, // TODO
      {spec.max_batch_size * 16 / 8, 8});
  EXPECT_TRUE(out_shape.find("Y2") == out_shape.end());
}

TEST(BoundShapeInference, ConcatMissingInput) {
  NetDef net;
  net.add_op()->CopyFrom(CreateOperatorDef(
      "Concat",
      "",
      {"I0", "I1"},
      {"Cout", "split_info"},
      {MakeArgument<int>("axis", 1), MakeArgument<int>("add_axis", 1)}));
  BoundShapeSpec spec(20, 1000);
  ShapeInfoMap shape_map;
  shape_map.emplace(
      "I0",
      makeTensorInfo(
          {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
          {spec.max_batch_size, 60}));
  BoundShapeInferencer eng(spec);
  eng.InferBoundShapeAndType(net, shape_map, nullptr);
  const auto& out_shape = eng.shape_info();
  verifyShapeInfo(
      out_shape,
      "I0",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 60});
  verifyShapeInfo(
      out_shape,
      "Cout",
      {TensorBoundShape_DimType_BATCH,
       TensorBoundShape_DimType_CONSTANT,
       TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 2, 60});
}

TEST(BoundShapeInference, Int8QuantizeInferInputBackwards) {
  NetDef net;
  net.add_op()->CopyFrom(CreateOperatorDef(
      "Int8Quantize",
      "",
      {"I0"},
      {"Cout", "split_info"},
      {MakeArgument<int>("Y_zero_point", 0),
       MakeArgument<float>("Y_scale", 0.05)}));
  net.add_op()->CopyFrom(CreateOperatorDef(
      "Int8FC",
      "",
      {"Cout", "W0", "B0"},
      {"Y"},
      {MakeArgument<int>("Y_zero_point", 0),
       MakeArgument<float>("Y_scale", 0.05)}));
  BoundShapeSpec spec(20, 1000);
  ShapeInfoMap shape_map;
  shape_map.emplace(
      "W0",
      makeTensorInfo(
          {TensorBoundShape_DimType_CONSTANT,
           TensorBoundShape_DimType_CONSTANT},
          {16, 101},
          TensorProto_DataType_UINT8,
          true));
  shape_map.emplace(
      "B0",
      makeTensorInfo(
          {TensorBoundShape_DimType_CONSTANT},
          {16},
          TensorProto_DataType_INT32,
          true));
  BoundShapeInferencer eng(spec);
  eng.InferBoundShapeAndType(net, shape_map, nullptr);
  const auto& out_shape = eng.shape_info();
  verifyShapeInfo(
      out_shape,
      "I0",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 101});
  verifyShapeInfo(
      out_shape,
      "Cout",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 101},
      TensorProto_DataType_UINT8,
      true);
  verifyShapeInfo(
      out_shape,
      "Y",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 16},
      TensorProto_DataType_UINT8,
      true);
}

TEST(BoundShapeInference, ConcatInferInputBackwards) {
  NetDef net;
  net.add_op()->CopyFrom(CreateOperatorDef(
      "Concat",
      "",
      {"I0", "I1"},
      {"Cout", "split_info"},
      {MakeArgument<int>("axis", 1)}));
  net.add_op()->CopyFrom(
      CreateOperatorDef("FCTransposed", "", {"Cout", "W0", "B0"}, {"Y"}, {}));
  BoundShapeSpec spec(20, 1000);
  ShapeInfoMap shape_map;
  shape_map.emplace(
      "I0",
      makeTensorInfo(
          {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
          {spec.max_batch_size, 60}));
  shape_map.emplace(
      "W0",
      makeTensorInfo(
          {TensorBoundShape_DimType_CONSTANT,
           TensorBoundShape_DimType_CONSTANT},
          {101, 16}));
  shape_map.emplace(
      "B0", makeTensorInfo({TensorBoundShape_DimType_CONSTANT}, {16}));
  BoundShapeInferencer eng(spec);
  eng.InferBoundShapeAndType(net, shape_map, nullptr);
  const auto& out_shape = eng.shape_info();
  verifyShapeInfo(
      out_shape,
      "I0",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 60});
  verifyShapeInfo(
      out_shape,
      "Cout",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 101});
  verifyShapeInfo(
      out_shape,
      "Y",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 16});
  verifyShapeInfo(
      out_shape,
      "I1",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 101 - 60});
}

TEST(BoundShapeInference, Bucketize) {
  NetDef net;
  net.add_op()->CopyFrom(CreateOperatorDef(
      "Bucketize",
      "",
      {"In"},
      {"Out"},
      {MakeArgument<std::vector<float>>("boundaries", {1.0, 2.0})}));
  BoundShapeSpec spec(20, 1000);
  ShapeInfoMap shape_map;
  shape_map.emplace(
      "In",
      makeTensorInfo(
          {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
          {spec.max_batch_size, 60}));
  BoundShapeInferencer eng(spec);
  eng.InferBoundShapeAndType(net, shape_map, nullptr);
  const auto& out_shape = eng.shape_info();
  verifyShapeInfo(
      out_shape,
      "Out",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 60},
      TensorProto_DataType_INT32);
}

TEST(BoundShapeInference, Split) {
  NetDef net;
  net.add_op()->CopyFrom(CreateOperatorDef(
      "Split", "", {"X"}, {"Y0", "Y1"}, {MakeArgument<int>("axis", 1)}));
  net.add_op()->CopyFrom(CreateOperatorDef(
      "Split",
      "",
      {"X"},
      {"Y2", "Y3", "Y4"},
      {MakeArgument<int>("axis", 1),
       MakeArgument<std::vector<int>>("split", {4, 30, 14})}));
  net.add_op()->CopyFrom(CreateOperatorDef(
      "Split",
      "",
      {"X1"},
      {"Y5", "Y6"},
      {MakeArgument<int>("axis", 1), MakeArgument<int>("add_axis", 1)}));
  BoundShapeSpec spec(20, 1000);
  ShapeInfoMap shape_map;
  shape_map.emplace(
      "X",
      makeTensorInfo(
          {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
          {spec.max_batch_size, 48}));
  shape_map.emplace(
      "X1",
      makeTensorInfo(
          {TensorBoundShape_DimType_BATCH,
           TensorBoundShape_DimType_CONSTANT,
           TensorBoundShape_DimType_CONSTANT},
          {spec.max_batch_size, 2, 48}));
  BoundShapeInferencer eng(spec);
  eng.InferBoundShapeAndType(net, shape_map, nullptr);
  const auto& out_shape = eng.shape_info();
  verifyShapeInfo(
      out_shape,
      "X",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 48});
  verifyShapeInfo(
      out_shape,
      "X1",
      {TensorBoundShape_DimType_BATCH,
       TensorBoundShape_DimType_CONSTANT,
       TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 2, 48});
  verifyShapeInfo(
      out_shape,
      "Y0",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 48 / 2});
  verifyShapeInfo(
      out_shape,
      "Y1",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 48 / 2});
  verifyShapeInfo(
      out_shape,
      "Y2",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 4});
  verifyShapeInfo(
      out_shape,
      "Y3",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 30});
  verifyShapeInfo(
      out_shape,
      "Y4",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 14});
  verifyShapeInfo(
      out_shape,
      "Y5",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 48});
  verifyShapeInfo(
      out_shape,
      "Y6",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 48});
}

TEST(BoundShapeInference, FC) {
  NetDef net;
  net.add_op()->CopyFrom(
      CreateOperatorDef("FC", "", {"X0", "W0", "B0"}, {"Out0"}, {}));
  net.add_op()->CopyFrom(
      CreateOperatorDef("FCTransposed", "", {"X1", "W1", "B1"}, {"Out1"}, {}));
  ShapeInfoMap shape_map;
  shape_map.emplace(
      "W0",
      makeTensorInfo(
          {TensorBoundShape_DimType_CONSTANT,
           TensorBoundShape_DimType_CONSTANT},
          {16, 1024}));
  shape_map.emplace(
      "B0", makeTensorInfo({TensorBoundShape_DimType_CONSTANT}, {16}));
  shape_map.emplace(
      "W1",
      makeTensorInfo(
          {TensorBoundShape_DimType_CONSTANT,
           TensorBoundShape_DimType_CONSTANT},
          {16, 1024}));
  shape_map.emplace(
      "B1", makeTensorInfo({TensorBoundShape_DimType_CONSTANT}, {1024}));
  BoundShapeSpec spec(20, 1000);
  BoundShapeInferencer eng(spec);
  eng.InferBoundShapeAndType(net, shape_map, nullptr);
  const auto& out_shape = eng.shape_info();
  verifyShapeInfo(
      out_shape,
      "X0",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 1024});
  verifyShapeInfo(
      out_shape,
      "Out0",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 16});
  verifyShapeInfo(
      out_shape,
      "X1",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 16});
  verifyShapeInfo(
      out_shape,
      "Out1",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 1024});
}

TEST(BoundShapeInference, FC3D) {
  NetDef net;
  net.add_op()->CopyFrom(
      CreateOperatorDef("FC", "", {"X0", "W0", "B0"}, {"Out0"}, {}));
  ShapeInfoMap shape_map;
  shape_map.emplace(
      "W0",
      makeTensorInfo(
          {TensorBoundShape_DimType_CONSTANT,
           TensorBoundShape_DimType_CONSTANT,
           TensorBoundShape_DimType_CONSTANT},
          {16, 1, 1024}));
  shape_map.emplace(
      "B0", makeTensorInfo({TensorBoundShape_DimType_CONSTANT}, {16}));
  BoundShapeSpec spec(20, 1000);
  BoundShapeInferencer eng(spec);
  eng.InferBoundShapeAndType(net, shape_map, nullptr);
  const auto& out_shape = eng.shape_info();
  verifyShapeInfo(
      out_shape,
      "X0",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 1024});
  verifyShapeInfo(
      out_shape,
      "Out0",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 16});
}

TEST(BoundShapeInference, Quantization) {
  NetDef net;
  net.add_op()->CopyFrom(CreateOperatorDef(
      "FloatToFused8BitRowwiseQuantized", "", {"w"}, {"Out_w"}, {}));
  ShapeInfoMap shape_map;
  shape_map.emplace(
      "w",
      makeTensorInfo(
          {TensorBoundShape_DimType_CONSTANT,
           TensorBoundShape_DimType_CONSTANT},
          {16, 64}));
  BoundShapeSpec spec(20, 1000);
  BoundShapeInferencer eng(spec);
  eng.InferBoundShapeAndType(net, shape_map, nullptr);
  const auto& out_shape = eng.shape_info();
  verifyShapeInfo(
      out_shape,
      "Out_w",
      {TensorBoundShape_DimType_CONSTANT, TensorBoundShape_DimType_CONSTANT},
      {16, 72},
      TensorProto_DataType_UINT8);
}

TEST(BoundShapeInference, Combo0) {
  NetDef net;
  net.add_op()->CopyFrom(CreateOperatorDef(
      "SparseLengthsSum", "", {"Weights0", "Data0", "Lengths0"}, {"EB0"}, {}));
  net.add_op()->CopyFrom(CreateOperatorDef(
      "SparseLengthsSum", "", {"Weights1", "Data1", "Lengths1"}, {"EB1"}, {}));
  net.add_op()->CopyFrom(CreateOperatorDef(
      "Concat",
      "",
      {"EB0", "EB1"},
      {"Cout", "split_info"},
      {MakeArgument<int>("axis", 1), MakeArgument<int>("add_axis", 1)}));
  net.add_op()->CopyFrom(CreateOperatorDef(
      "BatchMatMul",
      "",
      {"Cout", "Cout"},
      {"Bout"},
      {MakeArgument<int>("trans_b", 1)}));
  net.add_op()->CopyFrom(
      CreateOperatorDef("Flatten", "", {"Bout"}, {"Fout"}, {}));
  net.add_op()->CopyFrom(
      CreateOperatorDef("BatchGather", "", {"Fout", "Indices"}, {"Gout"}, {}));
  ShapeInfoMap shape_map;
  shape_map.emplace(
      "Weights0",
      makeTensorInfo(
          {TensorBoundShape_DimType_CONSTANT,
           TensorBoundShape_DimType_CONSTANT},
          {1000, 16}));
  shape_map.emplace(
      "Weights1",
      makeTensorInfo(
          {TensorBoundShape_DimType_CONSTANT,
           TensorBoundShape_DimType_CONSTANT},
          {20000, 16}));
  shape_map.emplace(
      "Indices", makeTensorInfo({TensorBoundShape_DimType_CONSTANT}, {2}));
  BoundShapeSpec spec(20, 1000);
  BoundShapeInferencer eng(spec);
  eng.InferBoundShapeAndType(net, shape_map, nullptr);
  const auto& out_shape = eng.shape_info();
  LOG(INFO) << eng.PrintShapeInfo();
  verifyShapeInfo(
      out_shape,
      "Gout",
      {TensorBoundShape_DimType_BATCH, TensorBoundShape_DimType_CONSTANT},
      {spec.max_batch_size, 2});
}
