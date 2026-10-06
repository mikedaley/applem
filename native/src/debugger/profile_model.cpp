/*
 * profile_model.cpp - A recording from the core's profiler, shaped for reading
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "debugger/profile_model.hpp"

#include <algorithm>

namespace a2e::native {

void ProfileModel::clear() {
  nodes_.clear();
  nested_.clear();
  framesCompleted_ = 0;
  frames_.clear();
  firstFrame_ = 0;
  range_.reset();
  self_.clear();
  total_.clear();
  calls_.clear();
  children_.clear();
  functions_.clear();
  functionIndex_.clear();
  time_ = 0;
  frameCount_ = 0;
  bandFunctions_.clear();
  frameBands_.clear();
}

void ProfileModel::setTree(std::vector<Node> nodes, uint64_t framesCompleted) {
  // A tree that shrank is a new recording: nothing known about the old one
  // holds.
  if (nodes.size() < nodes_.size()) nested_.clear();
  nodes_ = std::move(nodes);
  framesCompleted_ = framesCompleted;
  growCaches();
}

void ProfileModel::growCaches() {
  // Whether a node is a call made, however indirectly, from inside another
  // call to the same routine. Known for good once the node exists.
  for (size_t n = nested_.size(); n < nodes_.size(); n++) {
    bool nested = false;
    for (int32_t up = nodes_[n].parent; up > 0 && !nested; up = nodes_[static_cast<size_t>(up)].parent) {
      nested = nodes_[static_cast<size_t>(up)].function == nodes_[n].function;
    }
    nested_.push_back(nested ? 1 : 0);
  }
}

void ProfileModel::addFrames(std::vector<Frame> frames) {
  for (Frame &f : frames) {
    if (!frames_.empty() && f.index <= frames_.back().index) continue;
    frames_.push_back(std::move(f));
  }
  while (frames_.size() > Profiler::MAX_FRAMES) frames_.pop_front();
  if (!frames_.empty()) firstFrame_ = frames_.front().index;
}

void ProfileModel::setRange(std::optional<std::pair<uint64_t, uint64_t>> range) {
  if (range && range->first > range->second) std::swap(range->first, range->second);
  range_ = range;
}

void ProfileModel::setSelected(uint32_t address) { selected_ = address; }

void ProfileModel::rebuild() {
  const size_t count = nodes_.size();
  self_.assign(count, 0.0);
  calls_.assign(count, 0);
  time_ = 0;

  if (range_) {
    frameCount_ = 0;
    for (const Frame &f : frames_) {
      if (f.index < range_->first || f.index > range_->second) continue;
      frameCount_++;
      time_ += f.time;
      for (const Profiler::Sample &s : f.samples) {
        if (s.node >= count) continue;
        self_[s.node] += s.self;
        calls_[s.node] += s.calls;
      }
    }
  } else {
    for (size_t n = 0; n < count; n++) {
      self_[n] = nodes_[n].self;
      calls_[n] = nodes_[n].calls;
      time_ += nodes_[n].self;
    }
    frameCount_ = framesCompleted_;
  }

  // A child is always made after its parent, so walking backwards finishes
  // every subtree before its parent is added to.
  total_ = self_;
  for (size_t n = count; n-- > 1;) {
    total_[static_cast<size_t>(nodes_[n].parent)] += total_[n];
  }

  children_.assign(count, {});
  for (size_t n = 1; n < count; n++) {
    if (total_[n] > 0 || calls_[n] > 0) children_[static_cast<size_t>(nodes_[n].parent)].push_back(static_cast<uint32_t>(n));
  }
  for (auto &list : children_) {
    std::sort(list.begin(), list.end(), [this](uint32_t a, uint32_t b) { return total_[a] > total_[b]; });
  }

  functions_.clear();
  functionIndex_.clear();
  for (size_t n = 0; n < count; n++) {
    if (n > 0 && total_[n] <= 0 && calls_[n] == 0) continue;
    const Node &node = nodes_[n];
    auto [it, added] = functionIndex_.emplace(node.function, functions_.size());
    if (added) {
      Function f;
      f.address = node.function;
      f.entry = node.entry;
      functions_.push_back(f);
    }
    Function &f = functions_[it->second];
    if (node.entry == Profiler::Entry::Interrupt) f.entry = node.entry;
    f.self += self_[n];
    f.calls += calls_[n];
    if (!nested_[n]) f.total += total_[n];
  }
  std::sort(functions_.begin(), functions_.end(), [](const Function &a, const Function &b) {
    if (a.self != b.self) return a.self > b.self;
    return a.address < b.address;
  });
  functionIndex_.clear();
  bandFunctions_.clear();
  for (size_t i = 0; i < functions_.size(); i++) {
    functionIndex_[functions_[i].address] = i;
    if (i < BANDS && functions_[i].self > 0) {
      functions_[i].band = static_cast<int>(i);
      bandFunctions_.push_back(functions_[i].address);
    }
  }

  computeBands();
}

void ProfileModel::computeBands() {
  const size_t count = nodes_.size();
  // Each node's band, and whether it is inside a call to the selected
  // routine, worked out once for the tree rather than once per sample.
  std::vector<uint8_t> band(count, static_cast<uint8_t>(BANDS));
  std::vector<uint8_t> under(count, 0);
  for (size_t n = 0; n < count; n++) {
    const uint32_t f = nodes_[n].function;
    for (size_t b = 0; b < bandFunctions_.size(); b++) {
      if (bandFunctions_[b] == f) band[n] = static_cast<uint8_t>(b);
    }
    const bool parentUnder = n > 0 && under[static_cast<size_t>(nodes_[n].parent)];
    under[n] = (parentUnder || (selected_ != Profiler::TOP_LEVEL && f == selected_)) ? 1 : 0;
  }

  frameBands_.assign(frames_.size(), FrameBands{});
  for (size_t i = 0; i < frames_.size(); i++) {
    const Frame &frame = frames_[i];
    FrameBands &out = frameBands_[i];
    out.total = static_cast<float>(frame.time);
    for (const Profiler::Sample &s : frame.samples) {
      if (s.node >= count) continue;
      out.time[band[s.node]] += s.self;
      if (under[s.node]) out.selected += s.self;
    }
  }
}

const ProfileModel::Function *ProfileModel::function(uint32_t address) const {
  auto it = functionIndex_.find(address);
  return it == functionIndex_.end() ? nullptr : &functions_[it->second];
}

namespace {

std::vector<ProfileModel::Edge> sorted(std::unordered_map<uint32_t, ProfileModel::Edge> &edges) {
  std::vector<ProfileModel::Edge> out;
  out.reserve(edges.size());
  for (auto &[address, edge] : edges) out.push_back(edge);
  std::sort(out.begin(), out.end(), [](const ProfileModel::Edge &a, const ProfileModel::Edge &b) {
    if (a.time != b.time) return a.time > b.time;
    return a.address < b.address;
  });
  return out;
}

} // namespace

std::vector<ProfileModel::Edge> ProfileModel::callers(uint32_t address) const {
  std::unordered_map<uint32_t, Edge> edges;
  for (size_t n = 1; n < nodes_.size() && n < total_.size(); n++) {
    if (nodes_[n].function != address || nested_[n]) continue;
    if (total_[n] <= 0 && calls_[n] == 0) continue;
    const uint32_t from = nodes_[static_cast<size_t>(nodes_[n].parent)].function;
    Edge &e = edges[from];
    e.address = from;
    e.time += total_[n];
    e.calls += calls_[n];
  }
  return sorted(edges);
}

std::vector<ProfileModel::Edge> ProfileModel::callees(uint32_t address) const {
  std::unordered_map<uint32_t, Edge> edges;
  for (size_t n = 1; n < nodes_.size() && n < total_.size(); n++) {
    const Node &node = nodes_[n];
    if (node.function == address) continue;
    if (nodes_[static_cast<size_t>(node.parent)].function != address) continue;
    if (total_[n] <= 0 && calls_[n] == 0) continue;
    Edge &e = edges[node.function];
    e.address = node.function;
    e.time += total_[n];
    e.calls += calls_[n];
  }
  return sorted(edges);
}

} // namespace a2e::native
