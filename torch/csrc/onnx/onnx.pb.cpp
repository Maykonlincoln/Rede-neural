/* Automatically generated nanopb constant definitions */
/* Generated by nanopb-0.3.9-dev */

#include "onnx.pb.h"

/* @@protoc_insertion_point(includes) */
#if PB_PROTO_HEADER_VERSION != 30
#error Regenerate this file with the current version of nanopb generator.
#endif



const pb_field_t onnx_AttributeProto_fields[12] = {
    PB_FIELD(  1, STRING  , OPTIONAL, CALLBACK, FIRST, onnx_AttributeProto, name, name, 0),
    PB_FIELD(  2, FLOAT   , OPTIONAL, STATIC  , OTHER, onnx_AttributeProto, f, name, 0),
    PB_FIELD(  3, INT64   , OPTIONAL, STATIC  , OTHER, onnx_AttributeProto, i, f, 0),
    PB_FIELD(  4, BYTES   , OPTIONAL, CALLBACK, OTHER, onnx_AttributeProto, s, i, 0),
    PB_FIELD(  5, MESSAGE , OPTIONAL, CALLBACK, OTHER, onnx_AttributeProto, t, s, &onnx_TensorProto_fields),
    PB_FIELD(  6, MESSAGE , OPTIONAL, CALLBACK, OTHER, onnx_AttributeProto, g, t, &onnx_GraphProto_fields),
    PB_FIELD(  7, FLOAT   , REPEATED, CALLBACK, OTHER, onnx_AttributeProto, floats, g, 0),
    PB_FIELD(  8, INT64   , REPEATED, CALLBACK, OTHER, onnx_AttributeProto, ints, floats, 0),
    PB_FIELD(  9, BYTES   , REPEATED, CALLBACK, OTHER, onnx_AttributeProto, strings, ints, 0),
    PB_FIELD( 10, MESSAGE , REPEATED, CALLBACK, OTHER, onnx_AttributeProto, tensors, strings, &onnx_TensorProto_fields),
    PB_FIELD( 11, MESSAGE , REPEATED, CALLBACK, OTHER, onnx_AttributeProto, graphs, tensors, &onnx_GraphProto_fields),
    PB_LAST_FIELD
};

const pb_field_t onnx_NodeProto_fields[7] = {
    PB_FIELD(  1, STRING  , REPEATED, CALLBACK, FIRST, onnx_NodeProto, input, input, 0),
    PB_FIELD(  2, STRING  , REPEATED, CALLBACK, OTHER, onnx_NodeProto, output, input, 0),
    PB_FIELD(  3, STRING  , OPTIONAL, CALLBACK, OTHER, onnx_NodeProto, name, output, 0),
    PB_FIELD(  4, STRING  , OPTIONAL, CALLBACK, OTHER, onnx_NodeProto, op_type, name, 0),
    PB_FIELD(  5, MESSAGE , REPEATED, CALLBACK, OTHER, onnx_NodeProto, attribute, op_type, &onnx_AttributeProto_fields),
    PB_FIELD(  6, STRING  , OPTIONAL, CALLBACK, OTHER, onnx_NodeProto, doc_string, attribute, 0),
    PB_LAST_FIELD
};

const pb_field_t onnx_ModelProto_fields[8] = {
    PB_FIELD(  1, INT64   , OPTIONAL, STATIC  , FIRST, onnx_ModelProto, ir_version, ir_version, 0),
    PB_FIELD(  2, STRING  , OPTIONAL, CALLBACK, OTHER, onnx_ModelProto, producer_name, ir_version, 0),
    PB_FIELD(  3, STRING  , OPTIONAL, CALLBACK, OTHER, onnx_ModelProto, producer_version, producer_name, 0),
    PB_FIELD(  4, STRING  , OPTIONAL, CALLBACK, OTHER, onnx_ModelProto, domain, producer_version, 0),
    PB_FIELD(  5, INT64   , OPTIONAL, STATIC  , OTHER, onnx_ModelProto, model_version, domain, 0),
    PB_FIELD(  6, STRING  , OPTIONAL, CALLBACK, OTHER, onnx_ModelProto, doc_string, model_version, 0),
    PB_FIELD(  7, MESSAGE , OPTIONAL, STATIC  , OTHER, onnx_ModelProto, graph, doc_string, &onnx_GraphProto_fields),
    PB_LAST_FIELD
};

const pb_field_t onnx_GraphProto_fields[7] = {
    PB_FIELD(  1, MESSAGE , REPEATED, CALLBACK, FIRST, onnx_GraphProto, node, node, &onnx_NodeProto_fields),
    PB_FIELD(  2, STRING  , OPTIONAL, CALLBACK, OTHER, onnx_GraphProto, name, node, 0),
    PB_FIELD(  3, STRING  , REPEATED, CALLBACK, OTHER, onnx_GraphProto, input, name, 0),
    PB_FIELD(  4, STRING  , REPEATED, CALLBACK, OTHER, onnx_GraphProto, output, input, 0),
    PB_FIELD(  5, MESSAGE , REPEATED, CALLBACK, OTHER, onnx_GraphProto, initializer, output, &onnx_TensorProto_fields),
    PB_FIELD( 10, STRING  , OPTIONAL, CALLBACK, OTHER, onnx_GraphProto, doc_string, initializer, 0),
    PB_LAST_FIELD
};

const pb_field_t onnx_TensorProto_fields[10] = {
    PB_FIELD(  1, INT64   , REPEATED, CALLBACK, FIRST, onnx_TensorProto, dims, dims, 0),
    PB_FIELD(  2, UENUM   , OPTIONAL, STATIC  , OTHER, onnx_TensorProto, data_type, dims, 0),
    PB_FIELD(  3, MESSAGE , OPTIONAL, STATIC  , OTHER, onnx_TensorProto, segment, data_type, &onnx_TensorProto_Segment_fields),
    PB_FIELD(  4, FLOAT   , REPEATED, CALLBACK, OTHER, onnx_TensorProto, float_data, segment, 0),
    PB_FIELD(  5, INT32   , REPEATED, CALLBACK, OTHER, onnx_TensorProto, int32_data, float_data, 0),
    PB_FIELD(  6, BYTES   , REPEATED, CALLBACK, OTHER, onnx_TensorProto, string_data, int32_data, 0),
    PB_FIELD(  7, INT64   , REPEATED, CALLBACK, OTHER, onnx_TensorProto, int64_data, string_data, 0),
    PB_FIELD(  8, STRING  , OPTIONAL, CALLBACK, OTHER, onnx_TensorProto, name, int64_data, 0),
    PB_FIELD(  9, BYTES   , OPTIONAL, CALLBACK, OTHER, onnx_TensorProto, raw_data, name, 0),
    PB_LAST_FIELD
};

const pb_field_t onnx_TensorProto_Segment_fields[3] = {
    PB_FIELD(  1, INT64   , OPTIONAL, STATIC  , FIRST, onnx_TensorProto_Segment, begin, begin, 0),
    PB_FIELD(  2, INT64   , OPTIONAL, STATIC  , OTHER, onnx_TensorProto_Segment, end, begin, 0),
    PB_LAST_FIELD
};

const pb_field_t onnx_SparseTensorProto_fields[4] = {
    PB_FIELD(  1, INT64   , REPEATED, CALLBACK, FIRST, onnx_SparseTensorProto, dims, dims, 0),
    PB_FIELD(  2, MESSAGE , OPTIONAL, STATIC  , OTHER, onnx_SparseTensorProto, indices, dims, &onnx_TensorProto_fields),
    PB_FIELD(  3, MESSAGE , OPTIONAL, STATIC  , OTHER, onnx_SparseTensorProto, values, indices, &onnx_TensorProto_fields),
    PB_LAST_FIELD
};




/* Check that field information fits in pb_field_t */
#if !defined(PB_FIELD_32BIT)
/* If you get an error here, it means that you need to define PB_FIELD_32BIT
 * compile-time option. You can do that in pb.h or on compiler command line.
 * 
 * The reason you need to do this is that some of your messages contain tag
 * numbers or field sizes that are larger than what can fit in 8 or 16 bit
 * field descriptors.
 */
PB_STATIC_ASSERT((pb_membersize(onnx_ModelProto, graph) < 65536 && pb_membersize(onnx_TensorProto, segment) < 65536 && pb_membersize(onnx_SparseTensorProto, indices) < 65536 && pb_membersize(onnx_SparseTensorProto, values) < 65536), YOU_MUST_DEFINE_PB_FIELD_32BIT_FOR_MESSAGES_onnx_AttributeProto_onnx_NodeProto_onnx_ModelProto_onnx_GraphProto_onnx_TensorProto_onnx_TensorProto_Segment_onnx_SparseTensorProto)
#endif

#if !defined(PB_FIELD_16BIT) && !defined(PB_FIELD_32BIT)
/* If you get an error here, it means that you need to define PB_FIELD_16BIT
 * compile-time option. You can do that in pb.h or on compiler command line.
 * 
 * The reason you need to do this is that some of your messages contain tag
 * numbers or field sizes that are larger than what can fit in the default
 * 8 bit descriptors.
 */
PB_STATIC_ASSERT((pb_membersize(onnx_ModelProto, graph) < 256 && pb_membersize(onnx_TensorProto, segment) < 256 && pb_membersize(onnx_SparseTensorProto, indices) < 256 && pb_membersize(onnx_SparseTensorProto, values) < 256), YOU_MUST_DEFINE_PB_FIELD_16BIT_FOR_MESSAGES_onnx_AttributeProto_onnx_NodeProto_onnx_ModelProto_onnx_GraphProto_onnx_TensorProto_onnx_TensorProto_Segment_onnx_SparseTensorProto)
#endif


/* @@protoc_insertion_point(eof) */
