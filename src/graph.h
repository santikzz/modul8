#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "effect.h"

struct RenderPlan;

// one node on the canvas. an effect node owns its Effect; the two fixed io nodes
// (kInput / kOutput) have no effect. bypass is atomic so it can be toggled live.
struct GraphNode {
  int id;
  std::string type;  // effect registry name, or "in" / "out"
  std::unique_ptr<Effect> fx;
  std::atomic<bool> bypass{false};
  float x = 0.0f;
  float y = 0.0f;

  bool isIo() const { return !fx; }
};

// a directed cable: the output of `from` feeds the input bus of `to`.
struct GraphWire {
  int from;
  int to;
};

// the editable signal graph. nodes have a single summed input bus and a single
// fan-out output, which matches the mono buffers the effects work on: many wires
// into a node mix, one node feeding many wires splits. always holds an IN and OUT.
class Graph {
public:
  static constexpr int kInput = 0;
  static constexpr int kOutput = 1;

  Graph();

  GraphNode* addEffect(const std::string& type, float x, float y);
  // detaches the node and its wires and hands the node back so the caller can keep
  // it alive until the old render plan is retired, then let it free.
  std::unique_ptr<GraphNode> removeNode(int id);

  bool addWire(int from, int to);  // false on invalid / duplicate / cycle
  void removeWire(int from, int to);
  void raiseNode(int id);  // move to the top of the draw order
  // drop every effect node and wire, leaving the bare in -> out shell. used when a
  // preset is loaded so it can rebuild the rig from scratch. the engine must be
  // stopped first: the effects are freed immediately, not retired via a plan swap.
  void clear();

  int nodeCount() const { return (int)nodes_.size(); }

  GraphNode* node(int id);
  const std::vector<std::unique_ptr<GraphNode>>& nodes() const { return nodes_; }
  const std::vector<GraphWire>& wires() const { return wires_; }

  void prepareAll(double sampleRate, int block);
  std::unique_ptr<RenderPlan> buildPlan(int block) const;

private:
  bool wouldCycle(int from, int to) const;

  std::vector<std::unique_ptr<GraphNode>> nodes_;
  std::vector<GraphWire> wires_;
  int nextId_ = 2;
};
