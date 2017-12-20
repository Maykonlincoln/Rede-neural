#pragma once

#include <ATen/ATen.h>
#include <Python.h>

namespace torch { namespace utils {

PyObject* tensor_to_numpy_dtype(const at::Tensor& tensor);
PyObject* tensor_to_numpy(const at::Tensor& tensor);
at::Tensor tensor_from_numpy(PyObject* obj);

}} // namespace torch::utils
