/*
 * profile_model.hpp - A recording from the core's profiler, shaped for reading
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "../../../src/core/debug/profiler.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace a2e::native {

// What the Profiler window draws, worked out from the core's call tree and
// its frames. Plain C++ with no ImGui in it, so test_native_debugger pins
// the arithmetic.
//
// The core keeps time per path (a node); a person asks per routine. A
// routine's self time is the sum over every path that ends in it, and its
// total is the sum of those paths' subtrees, **except a path inside another
// call to the same routine**: a recursive routine's inner calls are already
// inside the outer call's subtree, and counting them again would put it
// above 100%.
//
// Everything can be looked at for the whole recording or for a range of
// frames. The whole recording is the core's running totals, which cover
// frames the timeline has already let go of; a range is summed from the
// frames.
class ProfileModel {
public:
  using Node = Profiler::Node;
  using Frame = Profiler::Frame;
  static constexpr size_t BANDS = 7; // routines with a colour of their own

  struct Function {
    uint32_t address = Profiler::TOP_LEVEL;
    Profiler::Entry entry = Profiler::Entry::TopLevel;
    double self = 0;
    double total = 0;
    uint64_t calls = 0;
    int band = -1; // its colour on the timeline, or -1 for "everything else"
  };

  // A caller or a callee of a routine, and the time that went that way.
  struct Edge {
    uint32_t address = 0;
    double time = 0;
    uint64_t calls = 0;
  };

  void clear();

  // The core's tree as it stands, and how many frames it has finished.
  // Nodes are only ever appended to it, so what was worked out about the
  // old ones is kept.
  void setTree(std::vector<Node> nodes, uint64_t framesCompleted);
  // Frames the core finished since the last call, in order.
  void addFrames(std::vector<Frame> frames);
  uint64_t nextFrame() const { return frames_.empty() ? firstFrame_ : frames_.back().index + 1; }
  const std::deque<Frame> &frames() const { return frames_; }
  // The machine's frame, in the profiler's units, for budgets.
  void setFrameTime(double time) { frameTime_ = time; }
  double frameTime() const { return frameTime_; }

  // Look at frames [first, last], or at everything.
  void setRange(std::optional<std::pair<uint64_t, uint64_t>> range);
  const std::optional<std::pair<uint64_t, uint64_t>> &range() const { return range_; }

  // Work everything out again for what is being looked at.
  void rebuild();

  // ===== After rebuild() =====

  const std::vector<Node> &nodes() const { return nodes_; }
  double nodeSelf(uint32_t n) const { return self_[n]; }
  double nodeTotal(uint32_t n) const { return total_[n]; }
  uint64_t nodeCalls(uint32_t n) const { return calls_[n]; }
  // A node's children, most time first, without the ones that took none.
  const std::vector<uint32_t> &children(uint32_t n) const { return children_[n]; }

  // Every routine that took any time, most self time first.
  const std::vector<Function> &functions() const { return functions_; }
  const Function *function(uint32_t address) const;
  // The time looked at, and how many frames that was.
  double time() const { return time_; }
  uint64_t frameCount() const { return frameCount_; }

  std::vector<Edge> callers(uint32_t address) const;
  std::vector<Edge> callees(uint32_t address) const;

  // The routines given a colour on the timeline, in band order: the ones
  // with the most self time.
  const std::vector<uint32_t> &bandFunctions() const { return bandFunctions_; }
  // One frame's time per band, the last element everything else, and the
  // share of the frame the selected routine took including what it called.
  struct FrameBands {
    std::array<float, BANDS + 1> time{};
    float selected = 0;
    float total = 0;
  };
  const std::vector<FrameBands> &frameBands() const { return frameBands_; }
  // The routine whose total the timeline traces; Profiler::TOP_LEVEL traces
  // none.
  void setSelected(uint32_t address);
  uint32_t selected() const { return selected_; }

private:
  void growCaches();
  void computeBands();

  std::vector<Node> nodes_;
  // Per node, fixed once known: whether a node above it is the same routine.
  std::vector<uint8_t> nested_;
  uint64_t framesCompleted_ = 0;
  std::deque<Frame> frames_;
  uint64_t firstFrame_ = 0;
  double frameTime_ = 0;
  std::optional<std::pair<uint64_t, uint64_t>> range_;

  std::vector<double> self_, total_;
  std::vector<uint64_t> calls_;
  std::vector<std::vector<uint32_t>> children_;
  std::vector<Function> functions_;
  std::unordered_map<uint32_t, size_t> functionIndex_;
  double time_ = 0;
  uint64_t frameCount_ = 0;

  std::vector<uint32_t> bandFunctions_;
  std::vector<FrameBands> frameBands_;
  uint32_t selected_ = Profiler::TOP_LEVEL;
};

} // namespace a2e::native
