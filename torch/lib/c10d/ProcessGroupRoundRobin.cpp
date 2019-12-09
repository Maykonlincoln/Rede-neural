#include <c10d/ProcessGroupRoundRobin.hpp>

namespace c10d {

ProcessGroupRoundRobin::ProcessGroupRoundRobin(
    int rank,
    int size,
    std::vector<std::shared_ptr<ProcessGroup>> processGroups)
    : ProcessGroup(rank, size), processGroups_(std::move(processGroups)) {
  TORCH_CHECK(processGroups_.size() >= 1);
  for (const auto& processGroup : processGroups_) {
    TORCH_CHECK(processGroup->getRank() == rank_);
    TORCH_CHECK(processGroup->getSize() == size_);
  }
  iterator_ = processGroups_.begin();
}

ProcessGroupRoundRobin::~ProcessGroupRoundRobin() {}

std::shared_ptr<ProcessGroup::Work> ProcessGroupRoundRobin::broadcast(
    std::vector<at::Tensor>& tensors,
    const BroadcastOptions& opts) {
  return next()->broadcast(tensors, opts);
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupRoundRobin::allreduce(
    std::vector<at::Tensor>& tensors,
    const AllreduceOptions& opts) {
  return next()->allreduce(tensors, opts);
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupRoundRobin::allreduce_coalesced(
    std::vector<at::Tensor>& tensors,
    const AllreduceCoalescedOptions& opts) {
  return next()->allreduce_coalesced(tensors, opts);
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupRoundRobin::reduce(
    std::vector<at::Tensor>& tensors,
    const ReduceOptions& opts) {
  return next()->reduce(tensors, opts);
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupRoundRobin::allgather(
    std::vector<std::vector<at::Tensor>>& outputs,
    std::vector<at::Tensor>& inputs,
    const AllgatherOptions& opts) {
  return next()->allgather(outputs, inputs, opts);
};

std::shared_ptr<ProcessGroup::Work> ProcessGroupRoundRobin::allgather_coalesced(
    std::vector<std::vector<at::Tensor>>& outputTensorLists,
    std::vector<at::Tensor>& inputTensors,
    const AllgatherOptions& opts) {
  return next()->allgather(outputTensorLists, inputTensors, opts);
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupRoundRobin::gather(
    std::vector<std::vector<at::Tensor>>& outputs,
    std::vector<at::Tensor>& inputs,
    const GatherOptions& opts) {
  return next()->gather(outputs, inputs, opts);
};

std::shared_ptr<ProcessGroup::Work> ProcessGroupRoundRobin::scatter(
    std::vector<at::Tensor>& outputs,
    std::vector<std::vector<at::Tensor>>& inputs,
    const ScatterOptions& opts) {
  return next()->scatter(outputs, inputs, opts);
};

std::shared_ptr<ProcessGroup::Work> ProcessGroupRoundRobin::reduce_scatter(
    std::vector<at::Tensor>& outputs,
    std::vector<std::vector<at::Tensor>>& inputs,
    const ReduceScatterOptions& opts) {
  return next()->reduce_scatter(outputs, inputs, opts);
};

std::shared_ptr<ProcessGroup::Work> ProcessGroupRoundRobin::send(
    std::vector<at::Tensor>& /* unused */,
    int /* unused */,
    int /* unused */) {
  throw std::runtime_error("ProcessGroupRoundRobin does not support send");
};

std::shared_ptr<ProcessGroup::Work> ProcessGroupRoundRobin::recv(
    std::vector<at::Tensor>& /* unused */,
    int /* unused */,
    int /* unused */) {
  throw std::runtime_error("ProcessGroupRoundRobin does not support recv");
};

std::shared_ptr<ProcessGroup::Work> ProcessGroupRoundRobin::recvAnysource(
    std::vector<at::Tensor>& /* unused */,
    int /* unused */) {
  throw std::runtime_error("ProcessGroupRoundRobin does not support recv");
};

std::shared_ptr<ProcessGroup::Work> ProcessGroupRoundRobin::barrier(
    const BarrierOptions& /* unused */) {
  throw std::runtime_error("ProcessGroupRoundRobin does not support barrier");
};

const std::shared_ptr<ProcessGroup>& ProcessGroupRoundRobin::next() {
  auto& processGroup = *iterator_;
  iterator_++;
  if (iterator_ == processGroups_.end()) {
    iterator_ = processGroups_.begin();
  }
  return processGroup;
}

} // namespace c10d
