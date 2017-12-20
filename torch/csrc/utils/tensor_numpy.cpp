#include "tensor_numpy.h"

#ifndef WITH_NUMPY
namespace torch { namespace utils {
PyObject* tensor_to_numpy(const at::Tensor& tensor) {
  throw std::runtime_error("PyTorch was compiled without NumPy support");
}
at::Tensor tensor_from_numpy(PyObject* obj) {
  throw std::runtime_error("PyTorch was compiled without NumPy support");
}
}}
#else

#include "torch/csrc/DynamicTypes.h"
#include "torch/csrc/Exceptions.h"

#include <ATen/ATen.h>
#include <memory>
#include <sstream>
#include <stdexcept>

#define NO_IMPORT_ARRAY
#define PY_ARRAY_UNIQUE_SYMBOL __numpy_array_api
#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#include <numpy/arrayobject.h>

using namespace at;

namespace torch { namespace utils {

static std::vector<npy_intp> to_numpy_shape(IntList x) {
  // shape and stride conversion from int64_t to npy_intp
  auto nelem = x.size();
  auto result = std::vector<npy_intp>(nelem);
  for (size_t i = 0; i < nelem; i++) {
    result[i] = static_cast<npy_intp>(x[i]);
  }
  return result;
}

static std::vector<int64_t> to_aten_shape(int ndim, npy_intp* values) {
  // shape and stride conversion from npy_intp to int64_t
  auto result = std::vector<int64_t>(ndim);
  for (int i = 0; i < ndim; i++) {
    result[i] = static_cast<int64_t>(values[i]);
  }
  return result;
}

static int aten_to_dtype(const at::Type& type);
static ScalarType dtype_to_aten(int dtype);

PyObject* tensor_to_numpy_dtype(const at::Tensor& tensor) {
  auto dtype = aten_to_dtype(tensor.type());
  return (PyObject *)PyArray_DescrFromType(dtype);
}

PyObject* tensor_to_numpy(const at::Tensor& tensor) {
  auto dtype = aten_to_dtype(tensor.type());
  auto sizes = to_numpy_shape(tensor.sizes());
  auto strides = to_numpy_shape(tensor.strides());
  // NumPy strides use bytes. Torch strides use element counts.
  auto element_size_in_bytes = tensor.type().elementSizeInBytes();
  for (auto& stride : strides) {
    stride *= element_size_in_bytes;
  }

  auto array = THPObjectPtr(PyArray_New(
      &PyArray_Type,
      tensor.dim(),
      sizes.data(),
      dtype,
      strides.data(),
      tensor.data_ptr(),
      0,
      NPY_ARRAY_ALIGNED | NPY_ARRAY_WRITEABLE,
      nullptr));
  if (!array) return NULL;

  // TODO: This attempts to keep the underlying memory alive by setting the base
  // object of the ndarray to the tensor and disabling resizes on the storage.
  // This is not sufficient. For example, the tensor's storage may be changed
  // via Tensor.set_, which can free the underlying memory.
  PyObject* py_tensor = createPyObject(tensor);
  if (!py_tensor) throw python_error();
  if (PyArray_SetBaseObject((PyArrayObject*)array.get(), py_tensor) == -1) {
    return NULL;
  }
  tensor.storage()->clear_flag(Storage::RESIZABLE);

  return array.release();
}

at::Tensor tensor_from_numpy(PyObject* obj) {
  if (!PyArray_Check(obj)) {
    throw TypeError("expected np.ndarray (got %s)", Py_TYPE(obj)->tp_name);
  }

  auto array = (PyArrayObject*)obj;
  int ndim = PyArray_NDIM(array);
  auto sizes = to_aten_shape(ndim, PyArray_DIMS(array));
  auto strides = to_aten_shape(ndim, PyArray_STRIDES(array));
  // NumPy strides use bytes. Torch strides use element counts.
  auto element_size_in_bytes = PyArray_ITEMSIZE(array);
  for (auto& stride : strides) {
    stride /= element_size_in_bytes;
  }

  size_t storage_size = 1;
  for (int i = 0; i < ndim; i++) {
    if (strides[i] < 0) {
      throw ValueError(
          "some of the strides of a given numpy array are negative. This is "
          "currently not supported, but will be added in future releases.");
    }
    // XXX: this won't work for negative strides
    storage_size += (sizes[i] - 1) * strides[i];
  }

  void* data_ptr = PyArray_DATA(array);
  auto& type = CPU(dtype_to_aten(PyArray_TYPE(array)));
  Py_INCREF(obj);
  return type.tensorFromBlob(data_ptr, sizes, strides, [obj](void* data) {
    AutoGIL gil;
    Py_DECREF(obj);
  });
}

static int aten_to_dtype(const at::Type& type) {
  if (type.is_cuda()) {
    throw TypeError(
        "can't convert CUDA tensor to numpy. Use Tensor.cpu() to "
        "copy the tensor to host memory first.");
  }
  if (type.is_sparse()) {
    throw TypeError(
        "can't convert sparse tensor to numpy. Use Tensor.to_dense() to "
        "convert to a dense tensor first.");
  }
  if (type.backend() == kCPU) {
    switch (type.scalarType()) {
      case kDouble: return NPY_DOUBLE;
      case kFloat: return NPY_FLOAT;
      case kHalf: return NPY_HALF;
      case kLong: return NPY_INT64;
      case kInt: return NPY_INT32;
      case kShort: return NPY_INT16;
      case kByte: return NPY_UINT8;
      default: break;
    }
  }
  throw TypeError("NumPy conversion for %s is not supported", type.toString());
}

static ScalarType dtype_to_aten(int dtype) {
  switch (dtype) {
    case NPY_DOUBLE: return kDouble;
    case NPY_FLOAT: return kFloat;
    case NPY_HALF: return kHalf;
    case NPY_INT64: return kLong;
    case NPY_INT32: return kInt;
    case NPY_INT16: return kShort;
    case NPY_UINT8: return kByte;
    default: break;
  }
  auto pytype = THPObjectPtr(PyArray_TypeObjectFromType(dtype));
  if (!pytype) throw python_error();
  throw TypeError(
      "can't convert np.ndarray of type %s. The only supported types are: "
      "double, float, float16, int64, int32, and uint8.",
      ((PyTypeObject*)pytype.get())->tp_name);
}

}} // namespace torch::utils

#endif  // WITH_NUMPY
