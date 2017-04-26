#pragma once

#include <Python.h>
#include <memory>
#include <typeinfo>

#include "torch/csrc/autograd/function.h"
#include "torch/csrc/utils/object_ptr.h"
#include "torch/csrc/Exceptions.h"

namespace torch { namespace autograd {

struct THPCppFunction {
  PyObject_HEAD
  std::shared_ptr<Function> cdata;
  int num_args;
  int num_required_args;
};

template<typename Ctor, int num_args, int num_required_args>
PyObject* CppFunction_pynew(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
  THPObjectPtr obj = type->tp_alloc(type, 0);
  if (!obj) return NULL;
  THPCppFunction* f = (THPCppFunction*)obj.get();
  f->num_args = num_args;
  f->num_required_args = num_required_args;
  HANDLE_TH_ERRORS
  new (&f->cdata) std::shared_ptr<Function>(Ctor()(args));
  END_HANDLE_TH_ERRORS
  if (!f->cdata) {
    return NULL;
  }
  return obj.release();
}

PyTypeObject* _initFunctionPyTypeObject(PyTypeObject& type, const char* name);

PyObject* registerFunctionHook(Function& fn, PyObject* hook);

template<typename Ctor, int args, int required_args>
PyTypeObject* createForwardFunctionPyTypeObject(PyTypeObject& type, const char* name)
{
  type.tp_new = &CppFunction_pynew<Ctor, args, required_args>;
  return _initFunctionPyTypeObject(type, name);
}

void registerCppFunction(const std::type_info& type, PyTypeObject* pytype);
PyObject* functionToPyObject(std::shared_ptr<Function> cdata);

}} // namespace torch::autograd
