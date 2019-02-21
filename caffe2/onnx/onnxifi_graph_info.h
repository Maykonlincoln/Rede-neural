#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "caffe2/core/logging.h"
#include "onnx/onnxifi_loader.h"

namespace caffe2 {
namespace onnx {

struct BackendGraphInfo {
  onnxBackendID backend_id;
  onnxBackend backend;
  onnxGraph graph;
  onnxifi_library* lib{nullptr};

  BackendGraphInfo(
      onnxBackendID backend_id,
      onnxBackend backend,
      onnxGraph graph,
      onnxifi_library* lib)
      : backend_id(backend_id), backend(backend), graph(graph), lib(lib) {}

  BackendGraphInfo(const BackendGraphInfo& other) = delete;

  BackendGraphInfo& operator=(const BackendGraphInfo& other) = delete;

  BackendGraphInfo(BackendGraphInfo&& other) noexcept {
    backend_id = other.backend_id;
    backend = other.backend;
    graph = other.graph;
    lib = other.lib;
    other.backend_id = other.backend = other.graph = other.lib = nullptr;
  }

  BackendGraphInfo& operator=(BackendGraphInfo&& other) {
    backend_id = other.backend_id;
    backend = other.backend;
    graph = other.graph;
    lib = other.lib;
    other.backend_id = other.backend = other.graph = other.lib = nullptr;
    return *this;
  }

  ~BackendGraphInfo() {
    if (lib) {
      onnxStatus err;
      if (graph) {
        err = lib->onnxReleaseGraph(graph);
        if (err != ONNXIFI_STATUS_SUCCESS) {
          LOG(ERROR) << "Error when calling onnxReleaseGraph";
        }
      }
      if (backend) {
        err = lib->onnxReleaseBackend(backend);
        if (err != ONNXIFI_STATUS_SUCCESS) {
          LOG(ERROR) << "Error when calling onnxReleaseBackend";
        }
      }
      if (backend_id) {
        err = lib->onnxReleaseBackendID(backend_id);
        if (err != ONNXIFI_STATUS_SUCCESS) {
          LOG(ERROR) << "Error when calling onnxReleaseBackendID";
        }
      }
    }
  }
};
using SharedPtrBackendGraphInfo = std::shared_ptr<BackendGraphInfo>;

// This class maintains a map of already created graph for nets+ops
class OnnxBackendGraphMap {
 public:
  OnnxBackendGraphMap() {}
  // Make class noncopyable and nomovable.
  OnnxBackendGraphMap(const OnnxBackendGraphMap&) = delete;
  OnnxBackendGraphMap(OnnxBackendGraphMap&&) = delete;
  OnnxBackendGraphMap operator=(const OnnxBackendGraphMap&) = delete;
  OnnxBackendGraphMap operator=(OnnxBackendGraphMap&&) = delete;

  SharedPtrBackendGraphInfo lookup(const std::string& key);

  // If acquisition of graph_ptr fails then graph already exists. And the
  // corresponding SharedPtrBackendGraphInfo is returned. Otherwise graph_ptr is
  // inserted in the map and the wrapper SharedPtrBackendGraphInfo is
  // returned.teebfhlbnheuk
  SharedPtrBackendGraphInfo insert(
      const std::string& key,
      BackendGraphInfo graph);

  void remove(const std::string& key);

 private:
  std::mutex backend_graph_map_lock_;
  std::unordered_map<std::string, SharedPtrBackendGraphInfo> backend_graph_map_;
};

OnnxBackendGraphMap* getOnnxBackendGraphMap();
} // namespace onnx
} // namespace caffe2
