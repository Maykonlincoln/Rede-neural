#ifndef TH_GENERIC_FILE
#define TH_GENERIC_FILE "generic/Tensor.cpp"
#else

#ifdef WITH_NUMPY

#ifdef TH_REAL_IS_DOUBLE
#define NUMPY_TYPE_ENUM NPY_DOUBLE
#endif
#ifdef TH_REAL_IS_FLOAT
#define NUMPY_TYPE_ENUM NPY_FLOAT
#endif
#ifdef TH_REAL_IS_LONG
#define NUMPY_TYPE_ENUM NPY_INT64
#endif
#ifdef TH_REAL_IS_INT
#define NUMPY_TYPE_ENUM NPY_INT32
#endif
#ifdef TH_REAL_IS_SHORT
#define NUMPY_TYPE_ENUM NPY_INT16
#endif
#ifdef TH_REAL_IS_BYTE
#define NUMPY_TYPE_ENUM NPY_UINT8
#endif

#endif

PyObject *THPTensorClass = NULL;
THPCopyList THTensor_(copy_functions);

PyObject * THPTensor_(NewEmpty)()
{
  return THPTensor_(New)(THTensor_(new)(LIBRARY_STATE_NOARGS));
}

PyObject * THPTensor_(New)(THTensor *tensor)
{
  THTensorPtr ptr(tensor);
  if (!tensor->storage) {
    tensor->storage = THStorage_(new)(LIBRARY_STATE_NOARGS);
  }
  PyTypeObject *type = (PyTypeObject *)THPTensorClass;
  PyObject *obj = type->tp_alloc(type, 0);
  if (obj) {
    ((THPTensor *)obj)->cdata = ptr.release();
  }
  return obj;
}

static THTensor* THPTensor_(_new)()
{
  THTensorPtr tensor(THTensor_(new)(LIBRARY_STATE_NOARGS));
  if (!tensor->storage) {
    tensor->storage = THStorage_(new)(LIBRARY_STATE_NOARGS);
  }
  return tensor.release();
}

static THTensor* THPTensor_(_newWithSize)(THLongStorage *size)
{
  THTensorPtr tensor(THTensor_(newWithSize)(LIBRARY_STATE size, NULL));
  // Ensure that PyTorch's "storage is not NULL" invariant is upheld
  // See Note [Storage is not NULL]
  if (!tensor->storage) {
    tensor->storage = THStorage_(new)(LIBRARY_STATE_NOARGS);
  }
  return tensor.release();
}

static void THPTensor_(dealloc)(THPTensor* self)
{
  THTensor_(free)(LIBRARY_STATE self->cdata);
  Py_TYPE(self)->tp_free((PyObject*)self);
}

static std::string THPTensor_(indicesToString)(std::vector<size_t> &indices,
    size_t depth)
{
  std::string index = "(";
  for (size_t i = 0; i <= depth; ++i) {
    index += std::to_string(indices[i]);
    index += ", ";
  }
  index.erase(index.length()-2);  // Remove trailing ", "
  index += ")";
  return index;
}

static void THPTensor_(setInconsistentDepthError)(std::vector<size_t> &sizes,
    std::vector<size_t> &indices, size_t depth, size_t length)
{
  std::string error = "inconsistent sequence length at index ";
  error += THPTensor_(indicesToString)(indices, depth);
  error += " - expected ";
  error += std::to_string(sizes[depth]);
  error += " but got ";
  error += std::to_string(length);
  THPUtils_setError(error.c_str());
}

#ifdef NUMPY_TYPE_ENUM
THTensor* THPTensor_(fromNumpy)(PyObject *numpy_array) {
  PyArrayObject *array = (PyArrayObject*)numpy_array;

  // Numpy and Torch disagree on empty tensors. In Torch, an empty
  // tensor is a tensor with zero dimensions. In Numpy, an empty tensor
  // keeps its shape, but has 0 as the size of one of the dimensions.
  // So we'll convert all Numpy tensors of 0 elements to empty Torch tensors.
  if (PyArray_SIZE(array) != 0) {
    auto ndim = PyArray_NDIM(array);
    size_t storage_size = 1;
    THLongStoragePtr sizes(THLongStorage_newWithSize(ndim));
    long *sizes_data = sizes->data;
    for (int i = 0; i < ndim; ++i) {
      sizes_data[i] = PyArray_DIM(array, i);
    }

    THLongStoragePtr strides(THLongStorage_newWithSize(ndim));
    long *strides_data = strides->data;
    for (int i = 0; i < ndim; ++i) {
      // numpy uses bytes, torch uses elements
      // we have to cast sizeof to long, because otherwise stride gets
      // promoted to size_t, and is UB for negative values
      strides_data[i] = PyArray_STRIDE(array, i) / ((long)sizeof(real));
      if (strides_data[i] < 0) {
        THPUtils_setError("some of the strides of a given numpy array are "
            "negative. This is currently not supported, but will be added in "
            "future releases.");
        return NULL;
      }
      // XXX: this won't work for negative strides
      storage_size += strides_data[i] * (sizes_data[i] - 1);
    }

    THStoragePtr storage(THStorage_(newWithDataAndAllocator)(
        (real*)PyArray_DATA(array),
        storage_size,
        &THNumpyArrayAllocator,
        new NumpyArrayAllocator(numpy_array)));
    THTensor *result = THTensor_(newWithStorage)(storage, 0, sizes, strides);
    return result;
  } else {
    THStoragePtr storage(THStorage_(new)());
    THTensor *result = THTensor_(newWithStorage)(storage, 0, NULL, NULL);
    return result;
  }
}
#endif

static PyObject * THPTensor_(pynew)(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
  HANDLE_TH_ERRORS
  Py_ssize_t num_args = args ? PyTuple_Size(args) : 0;

  THPTensorPtr self((THPTensor *)type->tp_alloc(type, 0));
  if (!self) {
    return NULL;
  }
  self->cdata = NULL;
#ifdef THC_GENERIC_FILE
  THCPAutoGPU gpu_guard;
#endif

  // Internally we allow constructing with a keyword only argument cdata
  if (kwargs != NULL) {
    Py_ssize_t num_kwargs = PyDict_Size(kwargs);
#ifdef THC_GENERIC_FILE
    PyObject *device_id = PyDict_GetItemString(kwargs, "device");
    if (device_id == Py_None) {
      num_kwargs--;
    } else if (device_id) {
      THPUtils_assert(THPUtils_checkLong(device_id), "device argument "
          " has to be an int, but got %s", THPUtils_typename(device_id));
      gpu_guard.setDevice(THPUtils_unpackLong(device_id));
      // simulate pop() and pretend this key was never there
      num_kwargs--;
    }
#endif
    if (num_args == 0) {
      PyObject *cdata_ptr = PyDict_GetItemString(kwargs, "cdata");
      if (num_kwargs == 1 && cdata_ptr && THPUtils_checkLong(cdata_ptr)) {
        THTensor *ptr = (THTensor*)PyLong_AsVoidPtr(cdata_ptr);
        self->cdata = ptr;
        return (PyObject*)self.release();
      }
    }
    // This is an internal option, so we don't want to advertise it.
#ifdef THC_GENERIC_FILE
    THPUtils_assert(num_kwargs == 0, THPTensorStr " constructor only "
        "accepts a 'device' keyword argument")
#else
    THPUtils_assert(num_kwargs == 0, THPTensorStr " constructor doesn't "
        "accept any keyword arguments");
#endif
  }

  // torch.Tensor()
  if (num_args == 0) {
    self->cdata = THPTensor_(_new)();
    return (PyObject*)self.release();
  }

  PyObject *first_arg = PyTuple_GET_ITEM(args, 0);

  // torch.Tensor(torch.Tensor tensor)
  if (num_args == 1 && THPTensor_(Check)(first_arg)) {
    THTensor *tensor = ((THPTensor*)first_arg)->cdata;
    self->cdata = THTensor_(newWithTensor)(LIBRARY_STATE tensor);
    return (PyObject*)self.release();
  }

  // torch.Tensor(torch.Size sizes)
  if (num_args == 1 && THPSize_Check(first_arg)) {
    THLongStoragePtr sizes(THPUtils_unpackSize(first_arg));
    self->cdata = THPTensor_(_newWithSize)(sizes.get());
    return (PyObject *)self.release();
  }

  // TODO: implement storageOffset, sizes and strides
  // torch.Tensor(torch.Storage data)
  if (num_args == 1 && THPStorage_(Check)(first_arg)) {
    THStorage *storage = ((THPStorage*)first_arg)->cdata;
    self->cdata = THTensor_(newWithStorage1d)(LIBRARY_STATE storage, 0, storage->size, -1);
    return (PyObject *)self.release();
  }

#ifdef NUMPY_TYPE_ENUM
  // torch.Tensor(np.ndarray array)
  if (num_args == 1 && PyArray_Check(first_arg) &&
      PyArray_TYPE((PyArrayObject*)first_arg) == NUMPY_TYPE_ENUM) {
    THPObjectPtr numpy_array(
      PyArray_FromArray((PyArrayObject*)first_arg, nullptr, NPY_ARRAY_BEHAVED));
    self->cdata = THPTensor_(fromNumpy)(numpy_array.get());
    if (!self->cdata)
        return NULL;
    return (PyObject*)self.release();
  }
#endif

  // torch.Tensor(Sequence data)
  if (num_args == 1 && PySequence_Check(first_arg)) {
    Py_ssize_t length = PySequence_Length(first_arg);
    THPUtils_assert(length >= 0, "couldn't obtain the length of %s",
        THPUtils_typename(first_arg));
    if (length == 0) {
      self->cdata = THPTensor_(_new)();
      return (PyObject*)self.release();
    }

    Py_INCREF(first_arg);
    THPObjectPtr item(first_arg);
    std::vector<size_t> sizes;
    while ((length = PySequence_Length(item)) >= 0) {
      sizes.push_back(length);
      // TODO: check for string in this case
      THPUtils_assert(sizes.size() < 1000000, "already counted a million "
          "dimensions in a given sequence. Most likely your items are also "
          "sequences and there's no way to infer how many dimension should "
          "the tensor have");
      THPUtils_assert(length > 0, "given sequence has an invalid size of "
          "dimension %ld: %ld", (long)sizes.size(), (long)length);
      item = PySequence_GetItem(item, 0);
      if (!item)
        return NULL;
    }
    // Last length check has set an error flag, so we need to clear it.
    PyErr_Clear();

    THLongStoragePtr sizes_storage(THLongStorage_newWithSize(sizes.size()));
    long *sizes_data = sizes_storage->data;
    for (auto size: sizes)
      *sizes_data++ = size;
    THTensorPtr tensor(THTensor_(newWithSize)(LIBRARY_STATE sizes_storage, NULL));

    int ndims = sizes.size();
    std::vector<size_t> indices(ndims);
    std::vector<THPObjectPtr> sequences(ndims);
    Py_INCREF(first_arg);
    item = first_arg;
    for (size_t i = 0; i < sequences.size(); i++) {
      PyObject *item_ptr = item.get();
      sequences[i] = std::move(item);
      if (i < sequences.size()-1) {
        item = PySequence_ITEM(item_ptr, 0);
        if (!item)
          return NULL;
      }
    }

    // half tensors don't have CPU counterparts so we have to buffer them as
    // floats while loading
#ifndef THC_REAL_IS_HALF
#define load_real real
#define UNPACK_REAL(item) THPUtils_(unpackReal)(item)
#else
#define load_real float
#define UNPACK_REAL(item) THPFloatUtils_unpackReal(item)
#endif
#if !defined(THC_GENERIC_FILE) && !defined(THD_GENERIC_FILE)
    real *data = tensor->storage->data;
#else
    size_t numel = THTensor_(numel)(LIBRARY_STATE tensor);
    std::unique_ptr<load_real> data_guard(new load_real[numel]);
    load_real *data = data_guard.get();
#endif
    THPObjectPtr final_sequence;
    while (true) {
      final_sequence = std::move(sequences[ndims-1]);
      try {
        // We're taking a fast-track over the last dimension
        for (size_t i = 0; i < sizes[ndims-1]; i++) {
          indices[ndims-1] = i;
          item = PySequence_ITEM(final_sequence, i);
          // We've checked the length earlier, so it must have been an error
          if (!item)
            return NULL;
          *data++ = UNPACK_REAL(item);
        }
      } catch(std::runtime_error &e) {
        std::string index = THPTensor_(indicesToString)(indices, ndims-1);
        THPUtils_setError("tried to construct a tensor from a %s%s sequence, "
            "but found an item of type %s at index %s",
            (ndims > 1 ? "nested " : ""),
            THPUtils_typeTraits<real>::python_type_str,
            THPUtils_typename(item.get()),
            index.c_str());
        return NULL;
      }
#ifdef THC_GENERIC_FILE
#ifdef THC_REAL_IS_HALF
      THFloatStorage *cpu_storage = THFloatStorage_newWithData(data_guard.get(), numel);
      cpu_storage->flag &= ~TH_STORAGE_FREEMEM;
      THCudaHalfStorage_copyFloat(LIBRARY_STATE tensor->storage, cpu_storage);
      THFloatStorage_free(cpu_storage);
#else
      THHostStorage *cpu_storage = THHostStorage_(newWithData)(data_guard.get(), numel);
      cpu_storage->flag &= ~TH_STORAGE_FREEMEM;
      THCStorage_(copyCPU)(LIBRARY_STATE tensor->storage, cpu_storage);
      THHostStorage_(free)(cpu_storage);
#endif
#endif
#undef UNPACK_REAL
#undef load_real

      // Update the counters
      int dim = ndims-2;
      size_t last_updated_dim = dim;
      while (dim >= 0) {
        last_updated_dim = dim;
        if (++indices[dim] == sizes[dim])
          indices[dim--] = 0;
        else
          break;
      }
      // Check if we've just made a full cycle
      if ((last_updated_dim == 0 && indices[0] == 0) || ndims == 1)
        break;
      // Update sequences
      for (int i = last_updated_dim+1; i < ndims; i++) {
        sequences[i] = PySequence_ITEM(sequences[i-1], indices[i-1]);
        if (!sequences[i]) {
          THPTensor_(setInconsistentDepthError)(sizes, indices, i, indices[i]);
          return NULL;
        }
        if (!PySequence_Check(sequences[i])) {
          std::string index_str = THPTensor_(indicesToString)(indices, i);
          THPUtils_setError("an item of time %s at index %s doesn't implement "
              "a sequence protocol");
          return NULL;
        }
        Py_ssize_t length = PySequence_Length(sequences[i]);
        if (length < 0) {
          std::string index_str = THPTensor_(indicesToString)(indices, i);
          THPUtils_setError("could not obtain a length of %s at index %s",
              THPUtils_typename(sequences[i].get()), index_str.c_str());
          return NULL;
        }
        if ((size_t)length != sizes[i]) {
          THPTensor_(setInconsistentDepthError)(sizes, indices, i, length);
          return NULL;
        }
      }
    }
    self->cdata = tensor.release();
    return (PyObject *)self.release();
  }

  // torch.Tensor(int ...)
  THLongStoragePtr sizes;
  if (THPUtils_tryUnpackLongVarArgs(args, 0, sizes)) {
    self->cdata = THPTensor_(_newWithSize)(sizes.get());
    return (PyObject *)self.release();
  }

  THPUtils_invalidArguments(args, kwargs, THPTensorStr " constructor", 6,
          "no arguments",
          "(int ...)",
          "(" THPTensorStr " viewed_tensor)",
          "(torch.Size size)",
          "(" THPStorageStr " data)",
          "(Sequence data)");
  return NULL;
  END_HANDLE_TH_ERRORS
}

#ifdef WITH_NUMPY
#define IS_SCALAR(NAME)                                                        \
  ((is_long = THPUtils_checkLong(NAME)) ||                                     \
   (is_scalar_array = PyArray_CheckScalar(NAME)))
#define UNPACK_SCALAR(IDX_VARIABLE)                                            \
  if (is_long) {                                                               \
    idx = THPUtils_unpackLong(IDX_VARIABLE);                                   \
  } else {                                                                     \
    PyArray_CastScalarToCtype(IDX_VARIABLE, &idx, NumpyLongArrDescr);          \
  }
#else
#define IS_SCALAR(NAME) THPUtils_checkLong(NAME)
#define UNPACK_SCALAR(IDX_VARIABLE) idx = THPUtils_unpackLong(IDX_VARIABLE);
#endif

#if defined(THC_GENERIC_FILE)
#define THIndexTensor THCudaLongTensor
#define THIndexTensor_(NAME) TH_CONCAT_2(THCudaLongTensor_,NAME)
#define THPIndexTensor THCPLongTensor
#define THPIndexTensor_Check THCPLongTensor_Check
#elif defined(THD_GENERIC_FILE)
#define THIndexTensor THDLongTensor
#define THIndexTensor_(NAME) TH_CONCAT_2(THDLongTensor_,NAME)
#define THPIndexTensor THDPLongTensor
#define THPIndexTensor_Check THDPLongTensor_Check
#else
#define THIndexTensor THLongTensor
#define THIndexTensor_(NAME) TH_CONCAT_2(THLongTensor_,NAME)
#define THPIndexTensor THPLongTensor
#define THPIndexTensor_Check THPLongTensor_Check
#endif

static bool THPTensor_(_indexOnce)(PyObject *index, int &indexed_dim,
        THTensorPtr &tresult, THStorage* &sresult, long &storage_offset)
{
#ifdef WITH_NUMPY
  static PyArray_Descr *NumpyLongArrDescr = PyArray_DescrFromType(NPY_INT64);
  bool is_long, is_scalar_array;
#endif
  // Indexing with a scalar
  if(IS_SCALAR(index)) {
    int64_t idx;
    UNPACK_SCALAR(index);
    long dimsize = THTensor_(size)(LIBRARY_STATE tresult.get(), indexed_dim);

    // If the user provided negative idx, convert to positive equivalent
    idx = (idx < 0) ? dimsize + idx : idx;

    if (dimsize <= 0) {
      PyErr_SetString(PyExc_IndexError, "indexing an empty tensor");
      throw python_error();
    }
    if (idx < 0 || idx >= dimsize) {
      PyErr_Format(PyExc_IndexError, "index %lld is out of range for dimension "
          "%lld (of size %lld)", (long long)idx, (long long)indexed_dim, (long long)dimsize);
      throw python_error();
    }

    // If we are indexing a vector, set the storage to the storage underlying
    // the vector, and the storage_offset to the location of the element at
    // the specificed index. Otherwise, perform a selection
    if(THTensor_(nDimension)(LIBRARY_STATE tresult.get()) == 1) {
      sresult = tresult.get()->storage;
      storage_offset = tresult->storageOffset + tresult->stride[0] * idx;
      tresult = NULL;
    } else {
      THTensor_(select)(LIBRARY_STATE tresult.get(), NULL, indexed_dim, idx);
    }
  } else if (index == Py_None) {
    // _indexOnce will never be called with tresult == NULL, except for a None index
    // e.g. x = torch.Tensor(5); y = x[5, None]
    if (!tresult) {
      tresult = THTensor_(newWithStorage1d)(LIBRARY_STATE sresult, storage_offset, 1, 1);
      sresult = NULL;
    } else {
      // Insert a singleton dimension at indexed_dim, then bump indexed_dim
      THTensor_(unsqueeze1d)(LIBRARY_STATE tresult.get(), NULL, indexed_dim++);
    }
  // Indexing with a slice
  } else if (PySlice_Check(index)) {
    Py_ssize_t start, end, length, step;
    if (!THPUtils_parseSlice(index, THTensor_(size)(LIBRARY_STATE tresult.get(), indexed_dim), &start, &end, &step, &length))
      throw python_error();
    if (step <= 0) {
      PyErr_SetString(PyExc_ValueError, "slice step has to be greater than 0");
      throw python_error();
    }
    if (length == 0) {
      PyErr_SetString(PyExc_ValueError, "result of slicing is an empty tensor");
      throw python_error();
    }
    // Modify the Tensor to point to the sliced components
    tresult->storageOffset += tresult->stride[indexed_dim] * start;
    tresult->stride[indexed_dim] *= step;
    tresult->size[indexed_dim] = length;
    indexed_dim++;
  } else {
    return false;
  }
  return true;
}

#ifndef TH_REAL_IS_HALF
static bool THPTensor_(_checkAdvancedIndexing)(THPTensor *indexed, PyObject *arg) {
  // Currently we only support the integer-array indexing strategy for advanced
  // indexing - where we have ndim sequence/LongTensor arguments

  long ndim = THTensor_(nDimension)(LIBRARY_STATE indexed->cdata);

  // Verify that all of the inputs are either Tensors or Sequences.
  if (PySequence_Check(arg) && PySequence_Size(arg) == ndim) {
    for (Py_ssize_t i = 0; i < ndim; ++i) {
      PyObject *item = PySequence_GetItem(arg, i);
      if (!THPIndexTensor_Check(item) && !PySequence_Check(item)) {
        Py_DECREF(item);
        return false;
      }
      Py_DECREF(item);
    }
    return true;
  }
  return false;

  // Full NumPy advanced indexing requirements are coded up below. To fully support
  // such indexing will require changes to the actual indexing logic, so we will
  // leave this commented out as a reference

  /**
  // Checks whether the specified selection object should trigger advanced
  // indexing

  // Case 1: arg is a non-tuple sequence object
  if (PySequence_Check(arg) && !PyTuple_Check(arg)) return true;

#ifdef WITH_NUMPY
  // Case 2: arg is an nd-array with type integer or bool
  if (PyArray_Check(arg) && (PyArray_TYPE((PyArrayObject*)arg) == NPY_INT64 || PyArray_TYPE((PyArrayObject*)arg) == NPY_BOOL)) return true;
#endif

  // Case 3: arg is a tuple containing at least one sequence object, ndarray, or LongTensor
  if (PyTuple_Check(arg)) {
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(arg); ++i) {
      PyObject *item = PyTuple_GET_ITEM(arg, i);
      if (PySequence_Check(item)) {
        return true;
      }
#ifdef WITH_NUMPY
      if (PyArray_Check(item) && (PyArray_TYPE((PyArrayObject*)item) == NPY_INT64 || PyArray_TYPE((PyArrayObject*)item) == NPY_BOOL)) return true;
#endif
      if (THPIndexTensor_Check(item)) return true;
    }
  }

  return false;
  **/
}

// Exposed at the interpreter level
static PyObject* THPTensor_(checkAdvancedIndexing)(THPTensor *self, PyObject *arg) {
  if (THPTensor_(_checkAdvancedIndexing)(self, arg)) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

static bool THPTensor_(_convertToTensorIndexers)(PyObject *index, std::vector<THIndexTensor*>& broadcasted) {
  // At the top-level, index must be a sequence. PySequence_Fast returns a new
  // reference
  PyObject *fast = PySequence_Fast(index, NULL);
  Py_ssize_t seqCount = PySequence_Fast_GET_SIZE(fast);
  std::vector<THPIndexTensor*> indexers;

  for (Py_ssize_t i = 0; i < seqCount; ++i) {
    THPIndexTensor *indexer = (THPIndexTensor *)PyObject_CallFunctionObjArgs(THPLongTensorClass, PySequence_Fast_GET_ITEM(fast, i), NULL);
    if (!indexer) {
      PyErr_Format(PyExc_IndexError,
          "When performing advanced indexing the indexing objects must be LongTensors or convertible to such");
      return false;
    }
    indexers.push_back(indexer);
  }
  Py_DECREF(fast);

  THIndexTensor **maybeBroadcasted = (THIndexTensor **)THAlloc(seqCount * sizeof(THIndexTensor*));
  THIndexTensor **candidates = (THIndexTensor **)THAlloc(seqCount * sizeof(THIndexTensor*));

  for (Py_ssize_t i = 0; i < seqCount; ++i) {
    maybeBroadcasted[i] = THIndexTensor_(new)(LIBRARY_STATE_NOARGS);
    candidates[i] = THIndexTensor_(newWithTensor)(LIBRARY_STATE indexers[i]->cdata);
  }

  // Broadcast/Expand indexing Tensors as necessary
  int ret = THIndexTensor_(expandNd)(LIBRARY_STATE maybeBroadcasted, candidates, seqCount, 0);

  // No matter what, need to cleanup the candidates
  for (Py_ssize_t i = 0; i < seqCount; ++i) {
    THIndexTensor_(free)(LIBRARY_STATE candidates[i]);
  }
  THFree(candidates);

  if (ret != 0) {
    // Broadcasted failed, cleanup and return error
    for (int i = 0; i < seqCount; ++i) {
      THIndexTensor_(free)(LIBRARY_STATE maybeBroadcasted[i]);
    }
    THFree(maybeBroadcasted);
    return false;
  }

  // Place Broadcasted Tensors into output vector, implicitly transferring
  // ownership
  for (Py_ssize_t i = 0; i < seqCount; ++i) {
    broadcasted.push_back(maybeBroadcasted[i]);
  }
  THFree(maybeBroadcasted);
  return true;
}

// Caller takes ownership of the returned IndexTensor
static THIndexTensor* THPTensor_(_calculateLinearIndices)(
    THTensorPtr& indexed, std::vector<THIndexTensor *>& broadcasted) {

  // Get the number of indices to generate - this will be equal to the number
  // of elements in each broadcasted Tensor
  ptrdiff_t indexingElements = THIndexTensor_(nElement)(LIBRARY_STATE broadcasted.at(0));
  THIndexTensor *linearIndices = THIndexTensor_(newWithSize1d)(LIBRARY_STATE indexingElements);
  THLongStorage *indexerSize = THLongStorage_newWithSize(1);
  THLongStorage_set(indexerSize, 0, indexingElements);

  for (ptrdiff_t i = 0; i < indexingElements; ++i) {
    long linearIdx = 0;
    for (int j = broadcasted.size() - 1; j >= 0; --j) {
      THIndexTensor *indexer = THIndexTensor_(newContiguous)(LIBRARY_STATE broadcasted.at(j));

      // The indexing tensor might not be one-dimensional, but we are generating a vector of
      // indices, so we need to view the indexer as 1D prior to getting the value for the
      // particular dimension
      THIndexTensor *oned = THIndexTensor_(newView)(LIBRARY_STATE indexer, indexerSize);
      linearIdx += THTensor_(stride)(LIBRARY_STATE indexed, j) * THIndexTensor_(get1d)(LIBRARY_STATE oned, i);
      THIndexTensor_(free)(LIBRARY_STATE oned);
      THIndexTensor_(free)(LIBRARY_STATE indexer);
    }
    THIndexTensor_(set1d)(LIBRARY_STATE linearIndices, i, linearIdx);
  }
  THLongStorage_free(indexerSize);
  return linearIndices;
}

static bool THPTensor_(_advancedIndexCommonInit)(
    PyObject *index,
    THTensorPtr &indexed,
    std::vector<THIndexTensor*>& broadcasted,
    THIndexTensor **linearIndices,
    THTensor **flattened) {

  // Precondition: index is an object that specifies advanced indexing.
  // For now, we only support the simple integer-array indexing strategy
  // where there are ndim(self) indexing sequences/LongTensors that can be
  // broadcasted and iterated as one
  // Precondition: tresult points to the Tensor we are indexing, and is also where
  // we will store the output Tensor

  // First attempt to convert to Tensor indexers from the arbitrary
  // python/tensor objects passed
  if (!THPTensor_(_convertToTensorIndexers)(index, broadcasted)) {
    return false;
  }

  // At this point broadcasted should store our indexing Tensors.
  // Our strategy is to view the indexed Tensor as a 1D Tensor, calculate
  // the linear indices for each tuple of indexing elements, and then call
  // indexSelect using those linear indices
  *linearIndices = THPTensor_(_calculateLinearIndices)(indexed, broadcasted);

  *flattened = THTensor_(newWithStorage1d)(LIBRARY_STATE
                                           THTensor_(storage)(LIBRARY_STATE indexed.get()),
                                           THTensor_(storageOffset)(LIBRARY_STATE indexed.get()),
                                           THTensor_(nElement)(LIBRARY_STATE indexed.get()),
                                           1);

  return true;
}

// Should called, written in such a way that if any of the parameters are not
// initialized we still don't crash
static void THPTensor_(_advancedIndexCommonCleanup)(
    std::vector<THIndexTensor*>& broadcasted,
    THIndexTensor *linearIndices,
    THTensor *flattened) {

  // Cleanup all TH memory
  for (const auto& bTensor : broadcasted) {
    THIndexTensor_(free)(LIBRARY_STATE bTensor);
  }
  if (linearIndices) THIndexTensor_(free)(LIBRARY_STATE linearIndices);
  if (flattened) THTensor_(free)(LIBRARY_STATE flattened);
}

static bool THPTensor_(_advancedIndexGet)(PyObject *index, THTensorPtr &tresult)
{
  std::vector<THIndexTensor*> broadcasted;
  THIndexTensor *linearIndices = NULL;
  THTensor *flattened = NULL;
  bool success = THPTensor_(_advancedIndexCommonInit)(
      index, tresult, broadcasted, &linearIndices, &flattened);

  if (success) {
    THTensor *result = THTensor_(new)(LIBRARY_STATE_NOARGS);

    // Index Select makes a copy of the storage, thus it is enforcing NumPy semantics, which says that the
    // array returned by advanced indexing is a copy, not a view
    THTensor_(indexSelect)(LIBRARY_STATE result, flattened, 0, linearIndices);

    // In the event that the indexing Tensors are not vectors, we need to reshape
    // the result to be the appropriate shape
    THTensor_(resizeNd)(LIBRARY_STATE result,
                        THIndexTensor_(nDimension)(LIBRARY_STATE broadcasted.at(0)),
                        broadcasted.at(0)->size,
                        broadcasted.at(0)->stride);

    // result ptr takes ownership of result tensor, and implicitly frees the
    // indexed one
    tresult = result;
  }

  THPTensor_(_advancedIndexCommonCleanup)(broadcasted, linearIndices, flattened);
  return success;
}

static bool THPTensor_(_advancedIndexSet)(PyObject *index, THTensorPtr &dest, PyObject *src)
{

  std::vector<THIndexTensor*> broadcasted;
  THIndexTensor *linearIndices = NULL;
  THTensor *flattened = NULL;
  bool success = THPTensor_(_advancedIndexCommonInit)(
      index, dest, broadcasted, &linearIndices, &flattened);

  if (success) {
    if (THPUtils_(checkReal)(src)) {
      real v = THPUtils_(unpackReal)(src);
      THTensor_(indexFill)(LIBRARY_STATE flattened, 0, linearIndices, v);
    } else if (THPTensor_(Check)(src)) {
      // Because we are doing an index copy, we need to make sure of two things:
      // 1. the src Tensor is 1D and
      // 2. the src is made contiguous before being flattened into a 1D view, if
      // necessary

      THTensor *contiguous;
      if (THTensor_(isContiguous(LIBRARY_STATE ((THPTensor*)src)->cdata))) {
        contiguous = THTensor_(newWithTensor)(LIBRARY_STATE ((THPTensor*)src)->cdata);
      } else {
        contiguous = THTensor_(newContiguous)(LIBRARY_STATE ((THPTensor*)src)->cdata);
      }

      THTensor *cviewed = THTensor_(newWithStorage1d)(LIBRARY_STATE
                                                      THTensor_(storage)(LIBRARY_STATE contiguous),
                                                      THTensor_(storageOffset)(LIBRARY_STATE contiguous),
                                                      THTensor_(nElement)(LIBRARY_STATE contiguous),
                                                      1);

      THTensor_(indexCopy)(LIBRARY_STATE flattened, 0, linearIndices, cviewed);
      THTensor_(free)(LIBRARY_STATE contiguous);
      THTensor_(free)(LIBRARY_STATE cviewed);
    } else {
      THPUtils_setError("can't assign %s to a " THPTensorStr " using a LongTensor "
          "(only " THPTensorStr " or %s are supported)",
          THPUtils_typename(src), THPUtils_typeTraits<real>::python_type_str);
      success = false;
    }
  }

  THPTensor_(_advancedIndexCommonCleanup)(broadcasted, linearIndices, flattened);
  return success;
}

static bool THPTensor_(_advancedIndexAdd)(PyObject *index, THTensorPtr &dest, THTensorPtr &src) {
  std::vector<THIndexTensor*> broadcasted;
  THIndexTensor *linearIndices = NULL;
  THTensor *flattened = NULL;
  bool success = THPTensor_(_advancedIndexCommonInit)(
      index, dest, broadcasted, &linearIndices, &flattened);

  if (success) {
    // Verify src tensor is contiguous before flattening
    THTensor *contiguous;
    if (THTensor_(isContiguous(LIBRARY_STATE src))) {
      contiguous = THTensor_(newWithTensor)(LIBRARY_STATE src);
    } else {
      contiguous = THTensor_(newContiguous)(LIBRARY_STATE src);
    }

    THTensor *cviewed = THTensor_(newWithStorage1d)(LIBRARY_STATE
                                                    THTensor_(storage)(LIBRARY_STATE contiguous),
                                                    THTensor_(storageOffset)(LIBRARY_STATE contiguous),
                                                    THTensor_(nElement)(LIBRARY_STATE contiguous),
                                                    1);

    THTensor_(indexAdd)(LIBRARY_STATE flattened, 0, linearIndices, cviewed);
    THTensor_(free)(LIBRARY_STATE contiguous);
    THTensor_(free)(LIBRARY_STATE cviewed);
  }

  THPTensor_(_advancedIndexCommonCleanup)(broadcasted, linearIndices, flattened);
  return success;
}

// Needed for autograd to support backwards passes when there are overlapping
// indices
static PyObject* THPTensor_(advancedIndexAdd)(THPTensor *self, PyObject *args) {
  HANDLE_TH_ERRORS

  THPUtils_assert(PyTuple_GET_SIZE(args) == 2, "advancedIndexAdd takes exactly two "
      "arguments (%d given)", (int) PyTuple_GET_SIZE(args));

  THPUtils_assert(THPTensor_(_checkAdvancedIndexing)(self, PyTuple_GET_ITEM(args, 0)),
      "first argument must be an indexer that triggers advanced indexing");

  THPUtils_assert(THPTensor_(Check)(PyTuple_GET_ITEM(args, 1)), "Second argument "
      "must be a Tensor");

  THTensorPtr gradOutput = THTensor_(newWithTensor)(
    LIBRARY_STATE ((THPTensor *)PyTuple_GET_ITEM(args, 1))->cdata);
  THTensorPtr dest = THTensor_(newWithTensor)(LIBRARY_STATE self->cdata);

  THPTensor_(_advancedIndexAdd)(PyTuple_GET_ITEM(args, 0), dest, gradOutput);

  Py_RETURN_NONE;
  END_HANDLE_TH_ERRORS
}
#endif // TH_REAL_IS_HALF

// Handles indexing into a Tensor given a tuple, ellipses, sequence, etc. index
static bool THPTensor_(_index)(THPTensor *self, PyObject *index,
    THTensorPtr &tresult, THStorage * &sresult, long &storage_offset)
{
  // As a base case, we create a new Tensor that is a copy of the Tensor
  // we are indexing
  tresult = THTensor_(newWithTensor)(LIBRARY_STATE self->cdata);
  sresult = NULL;
  int indexed_dim = 0;


  if(PyTuple_Check(index)) {
    // num_index_dim is the number of indices in the tuple, num_effective_index
    // is the number of non-None, non-ellipses indices
    long num_index_dim = (long)PyTuple_Size(index);
    long num_effective_index = num_index_dim;
    long num_tensor_dim = THTensor_(nDimension)(LIBRARY_STATE self->cdata);
    long ellipsis_idx = -1;
    for (int i = 0; i < num_index_dim; i++) {
      PyObject *dimidx = PyTuple_GET_ITEM(index, i);
      if (dimidx == Py_Ellipsis) {
        if (ellipsis_idx != -1) throw std::runtime_error("ellipsis can be used at most once");
        ellipsis_idx = i;
        num_effective_index--;
      }
      if (dimidx == Py_None) {
        num_effective_index--;
      }
    }
    if (num_effective_index > num_tensor_dim) {
      PyErr_Format(PyExc_IndexError,
          "trying to index %ld dimensions of a %ld dimensional tensor",
          num_effective_index, num_tensor_dim);
      return false;
    }

    // Loop through the indices and perform the indiviudal indexing at each dim
    bool valid = true;
    for (int dim = 0; dim < num_index_dim; dim++) {
      if (dim == ellipsis_idx) {
        // tresult can be NULL if ellipsis is the last item
        if (tresult) indexed_dim = tresult->nDimension - (num_index_dim - dim - 1);
        continue;
      }
      PyObject *dimidx = PyTuple_GET_ITEM(index, dim);
      valid = THPTensor_(_indexOnce)(dimidx, indexed_dim, tresult, sresult, storage_offset);
      if (!valid) {
        tresult = NULL;
        // overwrite this, so the message mentions the incorrect object
        index = dimidx;
        break;
      }
    }
    if (valid) return true;
  } else if (index == Py_Ellipsis) {
    // The result of indexing with an ellipsis only is just the entire existing
    // Tensor
    return true;
  } else {
    // index is a scalar, perform the indexing once on the 0th-dimension
    if (THPTensor_(_indexOnce)(index, indexed_dim, tresult, sresult, storage_offset))
      return true;
  }

  PyErr_Format(PyExc_TypeError, "indexing a tensor with an object of type %s. "
      "The only supported types are integers, slices"
#ifdef WITH_NUMPY
      ", numpy scalars and "
#endif
#ifndef THC_GENERIC_FILE
      "torch.LongTensor or torch.ByteTensor as the only argument.",
#else
      "torch.cuda.LongTensor or torch.cuda.ByteTensor as the only argument.",
#endif
    THPUtils_typename(index));
  return false;
}
#undef IS_SCALAR
#undef UNPACK_SCALAR

template<bool force_tensor>
static PyObject * THPTensor_(getValue)(THPTensor *self, PyObject *index)
{
  HANDLE_TH_ERRORS

#ifndef TH_REAL_IS_HALF
#if defined(THC_GENERIC_FILE)
  THCPByteTensor *mask = THCPByteTensor_Check(index) ? (THCPByteTensor*)index : NULL;
  THCPAutoGPU __gpu_guard(NULL, (PyObject*)self);
#elif defined(THD_GENERIC_FILE)
  THDPByteTensor *mask = THDPByteTensor_Check(index) ? (THDPByteTensor*)index : NULL;
#else
  THPByteTensor *mask = THPByteTensor_Check(index) ? (THPByteTensor*)index : NULL;
#endif
  if (mask) {
    THTensorPtr t(THTensor_(new)(LIBRARY_STATE_NOARGS));
    THTensor_(maskedSelect)(LIBRARY_STATE t.get(), self->cdata, mask->cdata);
    return THPTensor_(New)(t.release());
  }
  if (THPIndexTensor_Check(index)) {
    THIndexTensor *index_t = ((THPIndexTensor*)index)->cdata;
    THTensorPtr index_result(THTensor_(new)(LIBRARY_STATE_NOARGS));
    THTensor_(indexSelect)(LIBRARY_STATE index_result.get(), self->cdata, 0, index_t);
    return THPTensor_(New)(index_result.release());
  }
#endif

  THTensorPtr tresult;
  THStorage *sresult;
  long storage_offset;

  // Check and see if the indexing object triggers advanced indexing semantics
#ifndef TH_REAL_IS_HALF
  if (THPTensor_(_checkAdvancedIndexing)(self, index)) {
    tresult = THTensor_(newWithTensor)(LIBRARY_STATE self->cdata);
    if (!THPTensor_(_advancedIndexGet)(index, tresult)) {
      return NULL;
    }
    // TODO: needed?
    return THPTensor_(New)(tresult.release());
  }
#endif // TH_REAL_IS_HALF

  if (!THPTensor_(_index)(self, index, tresult, sresult, storage_offset))
    return NULL;
  if (tresult)
    return THPTensor_(New)(tresult.release());
  if (sresult) {
    if (force_tensor) {
      return THPTensor_(New)(THTensor_(newWithStorage1d)(LIBRARY_STATE sresult, storage_offset, 1, -1));
    } else {
      return THPUtils_(newReal)(THStorage_(get)(LIBRARY_STATE sresult, storage_offset));
    }
  }
  THPUtils_setError("An unknown error has occured when indexing a tensor "
      "in THPTensor_(getValue). Please report this in a github issue at: "
      "https://github.com/pytorch/pytorch");
  return NULL;
  END_HANDLE_TH_ERRORS
}

template<bool force_tensor>
static int THPTensor_(setValue)(THPTensor *self, PyObject *index, PyObject *value)
{
  HANDLE_TH_ERRORS

#ifndef TH_REAL_IS_HALF
#if defined(THC_GENERIC_FILE)
  THCPByteTensor *mask = THCPByteTensor_Check(index) ? (THCPByteTensor*)index : NULL;
  THCPAutoGPU __gpu_guard(NULL, (PyObject*)self);
#elif defined(THD_GENERIC_FILE)
  THDPByteTensor *mask = THDPByteTensor_Check(index) ? (THDPByteTensor*)index : NULL;
#else
  THPByteTensor *mask = THPByteTensor_Check(index) ? (THPByteTensor*)index : NULL;
#endif
  if (mask) {
    if (THPUtils_(checkReal)(value)) {
      real v = THPUtils_(unpackReal)(value);
      THTensor_(maskedFill)(LIBRARY_STATE self->cdata, mask->cdata, v);
    } else if (THPTensor_(Check)(value)) {
      THTensor_(maskedCopy)(LIBRARY_STATE self->cdata, mask->cdata, ((THPTensor*)value)->cdata);
    } else {
      THPUtils_setError("can't assign %s to a " THPTensorStr " using a mask "
          "(only " THPTensorStr " or %s are supported)",
          THPUtils_typename(value), THPUtils_typeTraits<real>::python_type_str);
    }
    return 0;
  }
  if (THPIndexTensor_Check(index)) {
    THIndexTensor *index_t = ((THPIndexTensor*)index)->cdata;
    if (THPUtils_(checkReal)(value)) {
      real v = THPUtils_(unpackReal)(value);
      THTensor_(indexFill)(LIBRARY_STATE self->cdata, 0, index_t, v);
    } else if (THPTensor_(Check)(value)) {
      THTensor_(indexCopy)(LIBRARY_STATE self->cdata, 0, index_t, ((THPTensor*)value)->cdata);
    } else {
      THPUtils_setError("can't assign %s to a " THPTensorStr " using a LongTensor "
          "(only " THPTensorStr " or %s are supported)",
          THPUtils_typename(value), THPUtils_typeTraits<real>::python_type_str);
    }
    return 0;
  }
#endif

  THTensorPtr tresult;
  THStorage *sresult;
  long storage_offset;

  // Check and see if the indexing object triggers advanced indexing semantics
#ifndef TH_REAL_IS_HALF
  if (THPTensor_(_checkAdvancedIndexing)(self, index)) {
    tresult = THTensor_(newWithTensor)(LIBRARY_STATE self->cdata);
    if (!THPTensor_(_advancedIndexSet)(index, tresult, value)) {
      return -1;
    }
    return 0;
  }

#endif // TH_REAL_IS_HALF
  if (!THPTensor_(_index)(self, index, tresult, sresult, storage_offset))
    return -1;
  if (sresult) {
    if (!force_tensor) {
      if (!THPUtils_(checkReal)(value)) {
        THPUtils_setError("can't assign a %s to a scalar value of type %s",
            THPUtils_typename(value), THPUtils_typeTraits<real>::python_type_str);
        return -1;
      }
      THStorage_(set)(LIBRARY_STATE sresult, storage_offset, THPUtils_(unpackReal)(value));
      return 0;
    } else {
      tresult = THTensor_(newWithStorage1d)(LIBRARY_STATE sresult, storage_offset, 1, -1);
    }
  }
  if (tresult) {
    if (THPUtils_(checkReal)(value)) {
#ifndef TH_REAL_IS_HALF
      THTensor_(fill)(LIBRARY_STATE tresult.get(), THPUtils_(unpackReal)(value));
#else
      throw std::runtime_error("torch.HalfTensors don't support scalar assignments");
#endif
    } else {
      // TODO: try to do this without creating a temporary object
      THPTensorPtr tmp((THPTensor*)THPTensor_(New)(tresult.release()));
      if (!tmp)
        return -1;
      if (!THPCopy(THTensor_(copy_functions), (PyObject*)tmp.get(), value, false, false)) {
        return -1;
      }
    }
    return 0;
  }
  THPUtils_setError("An unknown error has occured when indexing a tensor "
      "in THPTensor_(setValue). Please report this in a github issue at: "
      "https://github.com/pytorch/pytorch");
  return -1;
  END_HANDLE_TH_ERRORS_RET(-1)
}
#undef THIndexTensor
#undef THIndexTensor_
#undef THPIndexTensor
#undef THPIndexTensor_Check

Py_ssize_t THPTensor_(length)(THPTensor *self)
{
  if (self->cdata->nDimension == 0)
    return 0;
  return self->cdata->size[0];
}

#include "TensorMethods.cpp"

static PyMappingMethods THPTensor_(mappingmethods) = {
  (lenfunc)THPTensor_(length),
  (binaryfunc)THPTensor_(getValue)<false>,
  (objobjargproc)THPTensor_(setValue)<false>
};

// TODO: implement equality
PyTypeObject THPTensorType = {
  PyVarObject_HEAD_INIT(NULL, 0)
  "torch._C." THPTensorBaseStr,          /* tp_name */
  sizeof(THPTensor),                     /* tp_basicsize */
  0,                                     /* tp_itemsize */
  (destructor)THPTensor_(dealloc),       /* tp_dealloc */
  0,                                     /* tp_print */
  0,                                     /* tp_getattr */
  0,                                     /* tp_setattr */
  0,                                     /* tp_reserved */
  0,                                     /* tp_repr */
  0,                                     /* tp_as_number */
  0,                                     /* tp_as_sequence */
  &THPTensor_(mappingmethods),           /* tp_as_mapping */
  0,                                     /* tp_hash  */
  0,                                     /* tp_call */
  0,                                     /* tp_str */
  0,                                     /* tp_getattro */
  0,                                     /* tp_setattro */
  0,                                     /* tp_as_buffer */
  Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,   /* tp_flags */
  NULL,                                  /* tp_doc */
  0,                                     /* tp_traverse */
  0,                                     /* tp_clear */
  0,                                     /* tp_richcompare */
  0,                                     /* tp_weaklistoffset */
  0,                                     /* tp_iter */
  0,                                     /* tp_iternext */
  0,   /* will be assigned in init */    /* tp_methods */
  0,   /* will be assigned in init */    /* tp_members */
  0,                                     /* tp_getset */
  0,                                     /* tp_base */
  0,                                     /* tp_dict */
  0,                                     /* tp_descr_get */
  0,                                     /* tp_descr_set */
  0,                                     /* tp_dictoffset */
  0,                                     /* tp_init */
  0,                                     /* tp_alloc */
  THPTensor_(pynew),                     /* tp_new */
};

static struct PyMemberDef THPTensor_(members)[] = {
  {(char*)"_cdata", T_ULONGLONG, offsetof(THPTensor, cdata), READONLY, NULL},
  {NULL}
};

typedef struct {
  PyObject_HEAD
} THPTensorStateless;

PyTypeObject THPTensorStatelessType = {
  PyVarObject_HEAD_INIT(NULL, 0)
  "torch._C." THPTensorBaseStr ".stateless", /* tp_name */
  sizeof(THPTensorStateless),            /* tp_basicsize */
  0,                                     /* tp_itemsize */
  0,                                     /* tp_dealloc */
  0,                                     /* tp_print */
  0,                                     /* tp_getattr */
  0,                                     /* tp_setattr */
  0,                                     /* tp_reserved / tp_compare */
  0,                                     /* tp_repr */
  0,                                     /* tp_as_number */
  0,                                     /* tp_as_sequence */
  0,                                     /* tp_as_mapping */
  0,                                     /* tp_hash  */
  0,                                     /* tp_call */
  0,                                     /* tp_str */
  0,                                     /* tp_getattro */
  0,                                     /* tp_setattro */
  0,                                     /* tp_as_buffer */
  Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,   /* tp_flags */
  NULL,                                  /* tp_doc */
  0,                                     /* tp_traverse */
  0,                                     /* tp_clear */
  0,                                     /* tp_richcompare */
  0,                                     /* tp_weaklistoffset */
  0,                                     /* tp_iter */
  0,                                     /* tp_iternext */
  THPTensor_stateless_(methods),         /* tp_methods */
  0,                                     /* tp_members */
  0,                                     /* tp_getset */
  0,                                     /* tp_base */
  0,                                     /* tp_dict */
  0,                                     /* tp_descr_get */
  0,                                     /* tp_descr_set */
  0,                                     /* tp_dictoffset */
  0,                                     /* tp_init */
  0,                                     /* tp_alloc */
  0,                                     /* tp_new */
  0,                                     /* tp_free */
  0,                                     /* tp_is_gc */
  0,                                     /* tp_bases */
  0,                                     /* tp_mro */
  0,                                     /* tp_cache */
  0,                                     /* tp_subclasses */
  0,                                     /* tp_weaklist */
};

#if !defined(TH_REAL_IS_HALF) && !defined(THD_GENERIC_FILE)
#include "SparseTensor.cpp"
#endif

#ifndef THD_GENERIC_FILE
void THPTensor_(initCopyMethods)()
{
  auto& h = THTensor_(copy_functions);
  // copy from same type
  THPInsertTensorCopyFunction(h, &THTensor_(copy));
  // copy from CPU types
  THPInsertTensorCopyFunction(h, &THTensor_(copyByte));
  THPInsertTensorCopyFunction(h, &THTensor_(copyChar));
  THPInsertTensorCopyFunction(h, &THTensor_(copyShort));
  THPInsertTensorCopyFunction(h, &THTensor_(copyInt));
  THPInsertTensorCopyFunction(h, &THTensor_(copyLong));
  THPInsertTensorCopyFunction(h, &THTensor_(copyFloat));
  THPInsertTensorCopyFunction(h, &THTensor_(copyHalf));
  THPInsertTensorCopyFunction(h, &THTensor_(copyDouble));
#ifdef THC_GENERIC_FILE
  // copy from GPU types
  THPInsertTensorCopyFunction(h, &THTensor_(copyCudaByte));
  THPInsertTensorCopyFunction(h, &THTensor_(copyCudaChar));
  THPInsertTensorCopyFunction(h, &THTensor_(copyCudaShort));
  THPInsertTensorCopyFunction(h, &THTensor_(copyCudaInt));
  THPInsertTensorCopyFunction(h, &THTensor_(copyCudaLong));
  THPInsertTensorCopyFunction(h, &THTensor_(copyCudaFloat));
  THPInsertTensorCopyFunction(h, &THTensor_(copyCudaDouble));
#ifdef CUDA_HALF_TENSOR
  THPInsertTensorCopyFunction(h, &THTensor_(copyCudaHalf));
#endif
  THPInsertTensorCopyFunction(h, &THCTensor_(copyAsyncCPU), true);
  // add CPU <- GPU copies to base type
  #define THCpuTensor_(name) TH_CONCAT_4(TH, Real, Tensor_, name)
  extern THPCopyList THCpuTensor_(copy_functions);
  auto& b = THCpuTensor_(copy_functions);
  THPInsertTensorCopyFunction(b, &THCpuTensor_(copyCudaByte));
  THPInsertTensorCopyFunction(b, &THCpuTensor_(copyCudaChar));
  THPInsertTensorCopyFunction(b, &THCpuTensor_(copyCudaShort));
  THPInsertTensorCopyFunction(b, &THCpuTensor_(copyCudaInt));
  THPInsertTensorCopyFunction(b, &THCpuTensor_(copyCudaLong));
  THPInsertTensorCopyFunction(b, &THCpuTensor_(copyCudaFloat));
  THPInsertTensorCopyFunction(b, &THCpuTensor_(copyCudaDouble));
#ifdef CUDA_HALF_TENSOR
  THPInsertTensorCopyFunction(b, &THCpuTensor_(copyCudaHalf));
#endif
  THPInsertTensorCopyFunction(b, &THCpuTensor_(copyAsyncCuda), true);
  #undef THCpuTensor_
#endif
}
#else
void THPTensor_(initCopyMethods)()
{
  // TODO: cross type copies
  auto& h = THTensor_(copy_functions);
  THPInsertCopyFunction(h, &THDTensor_(copy));

  #define THCpuTensor_(name) TH_CONCAT_4(TH, Real, Tensor_, name)
  #define THCpuTensor TH_CONCAT_3(TH, Real, Tensor)
  #define THPCpuTensorType TH_CONCAT_3(THP, Real, TensorType)
  extern THPCopyList THCpuTensor_(copy_functions);
  auto& b = THCpuTensor_(copy_functions);

  THDPInsertCopyFunctionFromMaster(h, &THDTensor_(copyFromMaster), &THPCpuTensorType);
  THDPInsertCopyFunctionFromWorker(b, THDTensor_(copyFromWorker));

  #undef THCpuTensor
  #undef THCpuTensor_
  #undef THPCpuTensorType
}
#endif // !defined(THD_GENERIC_FILE)

bool THPTensor_(init)(PyObject *module)
{
#if !defined(THC_GENERIC_FILE) && !defined(TH_REAL_IS_HALF)
  THVector_(vectorDispatchInit)();
#endif
  THPTensorType.tp_methods = THPTensor_(methods);
  THPTensorType.tp_members = THPTensor_(members);
  if (PyType_Ready(&THPTensorType) < 0)
    return false;
  THPTensorStatelessType.tp_new = PyType_GenericNew;
  if (PyType_Ready(&THPTensorStatelessType) < 0)
    return false;

  PyModule_AddObject(module, THPTensorBaseStr, (PyObject *)&THPTensorType);
  THPTensor_(initCopyMethods)();
  return true;
}

bool THPTensor_(postInit)(PyObject *module)
{
  THPTensorClass = PyObject_GetAttrString(module,(char*)TH_CONCAT_STRING_2(Real,Tensor));
  if (!THPTensorClass) return false;

  bool is_cuda = false;
#ifdef THC_GENERIC_FILE
  is_cuda = true;
#endif
  const char *type_name = TH_CONCAT_STRING_2(Real,);
  torch::registerPyTypeObject((PyTypeObject*)THPTensorClass, type_name, is_cuda, false);
  return true;
}

#undef NUMPY_TYPE_ENUM

#endif
