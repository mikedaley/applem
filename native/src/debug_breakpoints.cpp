/*
 * debug_breakpoints.cpp - The debugger's breakpoints, as the user keeps them
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "debug_breakpoints.hpp"

#include "debug_symbols.hpp"

#include "debug/machine_debug.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace a2e::native {

namespace {

constexpr const char *KIND_NAMES[] = {"exec", "read", "write", "readwrite", "stack"};

} // namespace

const char *kindName(Breakpoint::Kind kind) { return KIND_NAMES[static_cast<int>(kind)]; }

std::optional<std::pair<uint32_t, uint32_t>> Breakpoints::parseRange(const std::string &text,
                                                                    const DebugSymbols &symbols,
                                                                    uint32_t addressMask) {
  // The dash between two ends; a name may not contain one, so the first
  // dash past the first character splits them.
  const size_t dash = text.find('-', 1);
  if (dash == std::string::npos) {
    const auto at = symbols.resolve(text, addressMask);
    if (!at) return std::nullopt;
    return std::make_pair(*at, *at);
  }
  const auto low = symbols.resolve(text.substr(0, dash), addressMask);
  const auto high = symbols.resolve(text.substr(dash + 1), addressMask);
  if (!low || !high || *high < *low) return std::nullopt;
  return std::make_pair(*low, *high);
}

bool Breakpoints::add(const Breakpoint &breakpoint) {
  for (const Breakpoint &b : list_) {
    if (b.kind == breakpoint.kind && b.start == breakpoint.start) return false;
  }
  list_.push_back(breakpoint);
  return true;
}

void Breakpoints::remove(size_t index) {
  if (index < list_.size()) list_.erase(list_.begin() + static_cast<long>(index));
}

bool Breakpoints::hasExecAt(uint32_t address) const {
  for (const Breakpoint &b : list_) {
    if (b.kind == Breakpoint::Kind::Exec && !b.isRange() && b.start == address) return true;
  }
  return false;
}

void Breakpoints::toggleExec(uint32_t address) {
  for (size_t i = 0; i < list_.size(); i++) {
    const Breakpoint &b = list_[i];
    if (b.kind == Breakpoint::Kind::Exec && !b.isRange() && b.start == address) {
      remove(i);
      return;
    }
  }
  Breakpoint b;
  b.start = b.end = address;
  list_.push_back(b);
}

int Breakpoints::execFor(uint32_t pc) const {
  // A single address before a range, so the narrower one is credited.
  for (size_t i = 0; i < list_.size(); i++) {
    const Breakpoint &b = list_[i];
    if (b.enabled && b.kind == Breakpoint::Kind::Exec && !b.isRange() && b.start == pc) return static_cast<int>(i);
  }
  for (size_t i = 0; i < list_.size(); i++) {
    const Breakpoint &b = list_[i];
    if (b.enabled && b.kind == Breakpoint::Kind::Exec && b.contains(pc)) return static_cast<int>(i);
  }
  return -1;
}

int Breakpoints::accessFor(uint32_t address, bool write) const {
  for (size_t i = 0; i < list_.size(); i++) {
    const Breakpoint &b = list_[i];
    if (!b.enabled || !b.contains(address)) continue;
    if (b.kind == Breakpoint::Kind::ReadWrite || (write ? b.kind == Breakpoint::Kind::Write
                                                        : b.kind == Breakpoint::Kind::Read)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int Breakpoints::stackFor(uint32_t low) const {
  for (size_t i = 0; i < list_.size(); i++) {
    const Breakpoint &b = list_[i];
    if (b.enabled && b.kind == Breakpoint::Kind::Stack && b.start == low) return static_cast<int>(i);
  }
  return -1;
}

void Breakpoints::apply(MachineDebug &debug, bool fresh) {
  if (!fresh) {
    for (const Breakpoint &b : applied_) {
      switch (b.kind) {
      case Breakpoint::Kind::Exec:
        if (b.isRange()) debug.removeBreakpointRange(b.start);
        else debug.removeBreakpoint(b.start);
        break;
      case Breakpoint::Kind::Stack: debug.removeStackBreakpoint(b.start); break;
      default: debug.removeWatchpoint(b.start); break;
      }
    }
  }
  applied_.clear();
  for (const Breakpoint &b : list_) {
    if (!b.enabled) continue;
    switch (b.kind) {
    case Breakpoint::Kind::Exec:
      if (b.isRange()) debug.addBreakpointRange(b.start, b.end);
      else debug.addBreakpoint(b.start);
      break;
    case Breakpoint::Kind::Stack: debug.addStackBreakpoint(b.start, b.end); break;
    case Breakpoint::Kind::Read: debug.addWatchpoint(b.start, b.end, MachineDebug::WP_READ); break;
    case Breakpoint::Kind::Write: debug.addWatchpoint(b.start, b.end, MachineDebug::WP_WRITE); break;
    case Breakpoint::Kind::ReadWrite: debug.addWatchpoint(b.start, b.end, MachineDebug::WP_READWRITE); break;
    }
    applied_.push_back(b);
  }
}

void Breakpoints::writeSettings(std::string &out) const {
  for (const Breakpoint &b : list_) {
    char head[96];
    std::snprintf(head, sizeof head, "Breakpoint=%s\t%X\t%X\t%d\t", kindName(b.kind), b.start, b.end,
                  b.enabled ? 1 : 0);
    out += head + b.condition + "\n";
  }
}

bool Breakpoints::readSetting(const char *line) {
  if (std::strncmp(line, "Breakpoint=", 11) != 0) return false;
  std::vector<std::string> parts(1);
  for (const char *c = line + 11; *c; c++) {
    if (*c == '\t') parts.emplace_back();
    else parts.back().push_back(*c);
  }
  if (parts.size() < 4) return true;
  Breakpoint b;
  bool known = false;
  for (int k = 0; k < 5; k++) {
    if (parts[0] == KIND_NAMES[k]) {
      b.kind = static_cast<Breakpoint::Kind>(k);
      known = true;
    }
  }
  if (!known) return true;
  b.start = static_cast<uint32_t>(std::strtoul(parts[1].c_str(), nullptr, 16));
  b.end = static_cast<uint32_t>(std::strtoul(parts[2].c_str(), nullptr, 16));
  if (b.end < b.start) b.end = b.start;
  b.enabled = parts[3] == "1";
  if (parts.size() > 4) b.condition = parts[4];
  add(b);
  return true;
}

} // namespace a2e::native
