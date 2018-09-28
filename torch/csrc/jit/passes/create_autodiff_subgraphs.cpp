#include "torch/csrc/jit/passes/create_autodiff_subgraphs.h"

#include "torch/csrc/jit/ir.h"
#include "torch/csrc/jit/autodiff.h"
#include "torch/csrc/jit/assertions.h"
#include "torch/csrc/jit/dynamic_dag.h"

#include <cstddef>
#include <limits>

namespace torch { namespace jit {

struct Graph;

namespace {

// Move nodes that exist in graph g into a 'group_node_kind' node.
// All inputs shared by the nodes become inputs to the new node.
// Outputs from 'nodes' are redirected to outputs of the new node,
// and the original nodes are removed.
// prereq: it is topologically valid to place the new node
// right before nodes[0] (i.e. it will not create cycles and all uses of
// new node will be after this position).
// prereq: nodes are in topological order
Node* mergeNodes(Block * block, Symbol group_node_kind, ArrayRef<Node*> nodes) {
  JIT_ASSERT(nodes.size() > 0);
  std::unordered_map<Value*, Value*> value_map;
  Graph * graph = block->owningGraph();

  auto new_graph = std::make_shared<Graph>();
  Node * group_node = graph->create(group_node_kind, 0);
  group_node->g_(attr::Subgraph, new_graph);

  auto getOrCreateInput = [&](Value * v) {
    if(value_map.count(v) > 0) {
      return value_map[v];
    }
    if (auto value = toIValue(v)) {
      Value * nv = new_graph->insertConstant(*value);
      value_map[v] = nv;
      return nv;
    }
    Value * nv = new_graph->addInput()->setType(v->type());
    group_node->addInput(v);
    value_map[v] = nv;
    return nv;
  };
  std::unordered_set<Node*> group_set(nodes.begin(), nodes.end());
  for(auto n : nodes) {
    auto nn = new_graph->appendNode(new_graph->createClone(n, getOrCreateInput));
    for(size_t i = 0; i < nn->outputs().size(); ++i) {
      auto old_output = n->outputs()[i];
      auto new_output = nn->outputs()[i];
      value_map[old_output] = new_output;
      std::vector<Use> to_replace;
      for(auto u : old_output->uses()) {
        // Uses within the set do not need to be made outputs
        if(group_set.count(u.user) > 0)
          continue;
        // Other uses do, but we
        // cannot replace them here or we invalid the uses list iterator
        to_replace.push_back(u);
      }
      if(to_replace.size() > 0) {
        new_graph->registerOutput(new_output);
        Value * external_output = group_node->addOutput()->setType(old_output->type());
        for(auto u : to_replace) {
          u.user->replaceInput(u.offset, external_output);
        }
      }
    }
  }
  group_node->insertBefore(nodes[0]);
  // delete backward, so that nodes are use-free before deletion
  for(size_t i = nodes.size(); i > 0; --i) {
    nodes[i - 1]->destroy();
  }
  JIT_ASSERT(isDifferentiable(*new_graph));
  return group_node;
}

// TODO(rzou): delete this
void CreateAutodiffSubgraphsNaive(
    Block * block,
    size_t threshold,
    std::vector<Node*>& diff_graphs) {
  // This implementation is not optimal, but it is simple.
  // It just scans through the list in order looking for runs of
  // differentiable ops, and then grouping them together when
  // it hits the first non-differentiable op.
  // It cannot handle things like:
  // a = f(x, y)
  // b = black_box(a)
  // c = g(a)
  // where you could group {f, g} together if the nodes were in a different
  // topological order

  // a better strategy would be to try to treat this like a fusion problem
  // and group maximal groups

  std::vector<Node*> groupable;
  for(Node * node : block->nodes()) { // Note: nodes() iterator stays valid since it is
                            // always pointing _after_ the nodes that mergeNodes
                            // mutates.
    if (isDifferentiable(node)) {
      // Constants are generally cheap to clone, so it's better to replicate them,
      // instead of moving them out from the original graph.
      if (node->kind() != prim::Constant) {
        groupable.push_back(node);
      }
    } else {
      if(groupable.size() >= threshold) {
        diff_graphs.push_back(mergeNodes(block, prim::DifferentiableGraph, groupable));
      }
      groupable.clear();
      for (Block * sub_block : node->blocks()) {
        CreateAutodiffSubgraphsNaive(sub_block, threshold, diff_graphs);
      }
    }
  }
  if(groupable.size() >= threshold) {
    diff_graphs.push_back(mergeNodes(block, prim::DifferentiableGraph, groupable));
  }
}

bool shouldConsiderForMerge(detail::Vertex<Node*>* v) {
  if (v->rdata.size() >= 2) {
    return true;
  }
  JIT_ASSERT(v->rdata.size() == 1);
  auto * node = *v->rdata.begin();
  if (node->kind() == prim::Constant) {
    return false;
  }
  return isDifferentiable(node);
}

static detail::DynamicDAG<Node*> make_dependency_graph(Block * block) {
  detail::DynamicDAG<Node*> dag;
  std::unordered_map<Node*,detail::Vertex<Node*>*> node_to_vertex;
  // NB: the block's param and return nodes are not in the dependency graph.
  for (Node * node : block->nodes()) {
    node_to_vertex[node] = dag.newVertex(node);
  }
  for (auto * node : block->nodes()) {
    for (auto * v : node->outputs()) {
      for (auto & use : v->uses()) {
        // [Determine data dependencies]
        // Consider the following code:
        //     y = f(x)
        //     if k:
        //        w += y
        //     z = g(y)
        // This produces a dependency graph with 3 vertices:
        // (0: f)   (1: if k ...)   (2: g)
        // We need to peek into the if Node* to determine its data dependencies
        // (the body depends on the output of f, so Vertex 1 depends on Vertex 0).
        // For each Use of y, we find an owning node of y that is a part of the
        // dependency graph (in this case, the Vertex containing the if Node*)
        // and then record the dependency.
        auto * owning_node = use.user;
        while (owning_node != nullptr) {
          auto search = node_to_vertex.find(owning_node);
          if (search == node_to_vertex.end()) {
            owning_node = owning_node->owningBlock()->owningNode();
            continue;
          }
          dag.addEdge(node_to_vertex[node], search->second);
          break;
        }
      }
    }
  }
  return dag;
}

static void find_differentiable_groups(
    detail::DynamicDAG<Node*>& dep_graph,
    size_t distance_threshold=64,
    size_t producer_outedge_threshold=16) {
  // A Vertex contains a Node* or a differential group of Node*.
  // Perform graph contraction on dep_graph: contract two vertices(x, y) if
  // the following conditions hold:
  // - x, y can be merged to form a differential group
  // - the contraction would not invalidate the dag (it creates no cycles).

  // Iterate in reverse topological order
  int64_t ord = dep_graph.maybe_vertices().size() - 1;
  while (ord >= 0) {
    if (!dep_graph.maybe_vertices().at(ord)) {
      --ord;
      continue;
    }

    auto * consumer = dep_graph.at(ord);
    if (!shouldConsiderForMerge(consumer)) {
      --ord;
      continue;
    }

    // Iterate through consumer->in_edges in reverse topological order
    detail::vertex_list<Node*> in_edges;
    in_edges.assign(consumer->in_edges.begin(), consumer->in_edges.end());
    dep_graph.sort(in_edges);

    bool changed = false;
    for (auto it = in_edges.rbegin(); it != in_edges.rend(); ++it) {
      auto * producer = *it;
      // The distance threshold makes this algorithm "not optimal": it will miss
      // some possible contraction opportunities, but it hopefully lets us:
      // 1) preserve locality of tensors. We don't want to keep them alive for too long.
      // 2) Bound the computation complexity for contractEdge
      if (consumer->ord - producer->ord > distance_threshold) continue;
      if (producer->out_edges.size() > producer_outedge_threshold) continue;
      if (!shouldConsiderForMerge(producer)) continue;

      // If the edge contraction is successful, consumer->in_edges
      // may have changed so we break out of this loop.
      if (dep_graph.contractEdge(producer, consumer)) {
        changed = true;
        break;
      }
    }
    // If we successfully contracted an edge, stay at this vertex.
    // It may have new edges that should be looked at before moving on.
    if (!changed) {
      --ord;
    }
  }
}

static void reorder_according_to_dag(Block * block, const detail::DynamicDAG<Node*>& dep_graph) {
  auto& vertices = dep_graph.maybe_vertices();
  for (auto it = vertices.begin(); it != vertices.end(); ++it) {
    if (!it->has_value()) continue;

    auto& rnodes = it->value()->rdata;
    for (auto it = rnodes.rbegin(); it != rnodes.rend(); ++it) {
      (*it)->moveBefore(block->return_node());
    }
  }
}

static void merge_differentiable_groups(
    Block * block,
    const detail::DynamicDAG<Node*>& dep_graph,
    size_t size_threshold,
    std::vector<Node*>& diff_graphs) {
  auto& vertices = dep_graph.maybe_vertices();
  for (auto it = vertices.begin(); it != vertices.end(); ++it) {
    if (!it->has_value()) continue;
    if (!shouldConsiderForMerge(it->value())) continue;

    auto& nodes = it->value()->rdata;
    if (nodes.size() < size_threshold) continue;

    std::reverse(std::begin(nodes), std::end(nodes));
    diff_graphs.push_back(mergeNodes(block, prim::DifferentiableGraph, nodes));
  }
}

void CreateAutodiffSubgraphsPK(
    Block * block,
    size_t size_threshold,
    std::vector<Node*>& diff_graphs) {
  for (auto * node : block->nodes()) {
    // Find subgraphs to run this on recursively.
    if (isDifferentiable(node)) continue;
    for (auto * sub_block : node->blocks()) {
      CreateAutodiffSubgraphsPK(sub_block, size_threshold, diff_graphs);
    }
  }

  auto dep_graph = make_dependency_graph(block);
  find_differentiable_groups(dep_graph);
  reorder_according_to_dag(block, dep_graph);
  merge_differentiable_groups(block, dep_graph, size_threshold, diff_graphs);
}

} // anonymous namespace

std::vector<Node*> CreateAutodiffSubgraphs(Graph & graph, size_t threshold) {
  std::vector<Node*> diff_nodes;
  CreateAutodiffSubgraphsPK(graph.block(), threshold, diff_nodes);
  return diff_nodes;
}

}}
