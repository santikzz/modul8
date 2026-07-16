#include "graph.h"

#include <algorithm>
#include <map>

#include "audio.h"
#include "registry.h"

Graph::Graph() {
  auto in = std::make_unique<GraphNode>();
  in->id = kInput;
  in->type = "in";
  in->x = 40.0f;
  in->y = 120.0f;
  nodes_.push_back(std::move(in));

  auto out = std::make_unique<GraphNode>();
  out->id = kOutput;
  out->type = "out";
  out->x = 560.0f;
  out->y = 120.0f;
  nodes_.push_back(std::move(out));

  addWire(kInput, kOutput);  // dry passthrough by default
}

GraphNode* Graph::node(int id) {
  for (auto& n : nodes_)
    if (n->id == id) return n.get();
  return nullptr;
}

GraphNode* Graph::addEffect(const std::string& type, float x, float y) {
  auto fx = Registry::create(type);
  if (!fx) return nullptr;

  auto n = std::make_unique<GraphNode>();
  n->id = nextId_++;
  n->type = type;
  n->fx = std::move(fx);
  n->x = x;
  n->y = y;
  GraphNode* raw = n.get();
  nodes_.push_back(std::move(n));
  return raw;
}

std::unique_ptr<GraphNode> Graph::removeNode(int id) {
  if (id == kInput || id == kOutput) return nullptr;

  wires_.erase(std::remove_if(wires_.begin(), wires_.end(),
                              [id](const GraphWire& w) { return w.from == id || w.to == id; }),
               wires_.end());

  for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
    if ((*it)->id == id) {
      auto dead = std::move(*it);
      nodes_.erase(it);
      return dead;
    }
  }
  return nullptr;
}

bool Graph::addWire(int from, int to) {
  if (from == to || to == kInput || from == kOutput) return false;
  if (!node(from) || !node(to)) return false;
  for (auto& w : wires_)
    if (w.from == from && w.to == to) return false;
  if (wouldCycle(from, to)) return false;
  wires_.push_back({from, to});
  return true;
}

void Graph::raiseNode(int id) {
  for (auto it = nodes_.begin(); it != nodes_.end(); ++it) {
    if ((*it)->id == id) {
      std::rotate(it, it + 1, nodes_.end());  // move this node to the back (drawn last)
      return;
    }
  }
}

void Graph::clear() {
  wires_.clear();
  nodes_.erase(std::remove_if(nodes_.begin(), nodes_.end(),
                              [](const std::unique_ptr<GraphNode>& n) {
                                return n->id != kInput && n->id != kOutput;
                              }),
               nodes_.end());
  nextId_ = 2;
}

void Graph::removeWire(int from, int to) {
  wires_.erase(std::remove_if(wires_.begin(), wires_.end(),
                              [&](const GraphWire& w) { return w.from == from && w.to == to; }),
               wires_.end());
}

// adding from->to closes a loop if `to` can already reach `from`.
bool Graph::wouldCycle(int from, int to) const {
  std::vector<int> stack = {to};
  std::vector<int> seen;
  while (!stack.empty()) {
    int cur = stack.back();
    stack.pop_back();
    if (cur == from) return true;
    if (std::find(seen.begin(), seen.end(), cur) != seen.end()) continue;
    seen.push_back(cur);
    for (auto& w : wires_)
      if (w.from == cur) stack.push_back(w.to);
  }
  return false;
}

void Graph::prepareAll(double sampleRate, int block) {
  for (auto& n : nodes_)
    if (n->fx) n->fx->prepare(sampleRate, block);
}

// flatten to a topologically ordered RenderPlan (kahn's algorithm). nodes are
// prevented from forming cycles on wiring, so every node lands in the order.
std::unique_ptr<RenderPlan> Graph::buildPlan(int block) const {
  auto plan = std::make_unique<RenderPlan>();
  int n = (int)nodes_.size();

  std::map<int, int> idx;  // node id -> buffer index
  for (int i = 0; i < n; i++) idx[nodes_[i]->id] = i;

  std::vector<int> indeg(n, 0);
  for (auto& w : wires_) indeg[idx[w.to]]++;

  std::vector<int> order;
  std::vector<int> ready;
  for (int i = 0; i < n; i++)
    if (indeg[i] == 0) ready.push_back(i);
  while (!ready.empty()) {
    int cur = ready.back();
    ready.pop_back();
    order.push_back(cur);
    for (auto& w : wires_) {
      if (idx[w.from] != cur) continue;
      int t = idx[w.to];
      if (--indeg[t] == 0) ready.push_back(t);
    }
  }
  if ((int)order.size() != n) return nullptr;  // cycle guard, should not happen

  plan->buffers.assign(n, std::vector<float>(block, 0.0f));
  plan->inputBuf = idx[kInput];
  plan->outputBuf = idx[kOutput];

  for (int oi : order) {
    RenderStep step;
    step.fx = nodes_[oi]->fx.get();
    step.bypass = nodes_[oi]->fx ? &nodes_[oi]->bypass : nullptr;
    step.outBuf = oi;
    for (auto& w : wires_)
      if (idx[w.to] == oi) step.inputs.push_back(idx[w.from]);
    plan->steps.push_back(std::move(step));
  }
  return plan;
}
