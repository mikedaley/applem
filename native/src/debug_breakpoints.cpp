/*
 * debug_breakpoints.cpp - The debugger's breakpoints, as the user keeps them
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "debug_breakpoints.hpp"

#include "debug_symbols.hpp"

#include "debug/machine_debug.hpp"
#include "debug/soft_switch_catalog.hpp"

#include <algorithm>
#include <cctype>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace a2e::native {

namespace {

constexpr const char *KIND_NAMES[] = {"exec", "read", "write", "readwrite", "stack", "switch", "beam"};
constexpr int KIND_COUNT = 7;

// What the core needs to know of a breakpoint; a change to anything else (a
// condition, a hit count) is the host's business and applies nothing.
bool sameInCore(const Breakpoint &a, const Breakpoint &b) {
  return a.kind == b.kind && a.start == b.start && a.end == b.end && a.enabled == b.enabled && a.key == b.key &&
         a.equals == b.equals && a.value == b.value && a.mask == b.mask && a.scanline == b.scanline &&
         a.hPos == b.hPos;
}

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

bool Breakpoint::same(const Breakpoint &other) const {
  if (kind != other.kind) return false;
  switch (kind) {
  case Kind::Switch:
    return key == other.key && equals == other.equals && (!equals || (value == other.value && mask == other.mask));
  case Kind::Beam: return scanline == other.scanline && hPos == other.hPos;
  default: return start == other.start;
  }
}

bool Breakpoints::add(const Breakpoint &breakpoint) {
  for (const Breakpoint &b : list_) {
    if (b.same(breakpoint)) return false;
  }
  list_.push_back(breakpoint);
  list_.back().coreId = -1;
  return true;
}

void Breakpoints::toggle(const Breakpoint &breakpoint) {
  for (size_t i = 0; i < list_.size(); i++) {
    if (list_[i].same(breakpoint)) {
      remove(i);
      return;
    }
  }
  add(breakpoint);
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

int Breakpoints::switchFor(int32_t coreId) const {
  for (size_t i = 0; i < list_.size(); i++) {
    if (list_[i].kind == Breakpoint::Kind::Switch && list_[i].coreId == coreId && coreId >= 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int Breakpoints::beamFor(int32_t coreId) const {
  for (size_t i = 0; i < list_.size(); i++) {
    if (list_[i].kind == Breakpoint::Kind::Beam && list_[i].coreId == coreId && coreId >= 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool Breakpoints::needsApply() const {
  if (stale_ || list_.size() != applied_.size()) return true;
  for (size_t i = 0; i < list_.size(); i++) {
    if (!sameInCore(list_[i], applied_[i])) return true;
  }
  return false;
}

// Everything handed over last time is taken back first. After a rebuild the
// core holds none of it, and taking back what is not there does nothing, so
// the same path serves both. The switch and beam breakpoints are only ever
// this list's, so those are cleared whole.
void Breakpoints::apply(MachineDebug &debug, const std::vector<SoftSwitchInfo> &switches) {
  for (const Breakpoint &b : applied_) {
    if (!b.enabled) continue;
    switch (b.kind) {
    case Breakpoint::Kind::Exec:
      if (b.isRange()) debug.removeBreakpointRange(b.start);
      else debug.removeBreakpoint(b.start);
      break;
    case Breakpoint::Kind::Stack: debug.removeStackBreakpoint(b.start); break;
    case Breakpoint::Kind::Read:
    case Breakpoint::Kind::Write:
    case Breakpoint::Kind::ReadWrite: debug.removeWatchpoint(b.start); break;
    case Breakpoint::Kind::Switch:
    case Breakpoint::Kind::Beam: break;
    }
  }
  debug.clearSwitchBreakpoints();
  debug.clearBeamBreakpoints();

  for (Breakpoint &b : list_) {
    b.coreId = -1;
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
    case Breakpoint::Kind::Switch:
      if (const SoftSwitchInfo *sw = findSoftSwitch(switches, b.key.c_str())) {
        const uint64_t mask = sw->isRegister() ? b.mask : sw->mask();
        const uint64_t value = sw->isRegister() ? b.value : (b.value ? mask : 0);
        b.coreId = debug.addSwitchBreakpoint(sw->source, mask,
                                             b.equals ? MachineDebug::SwitchCondition::Equals
                                                      : MachineDebug::SwitchCondition::Changes,
                                             value);
      }
      break;
    case Breakpoint::Kind::Beam:
      b.coreId = debug.addBeamBreakpoint(static_cast<int16_t>(b.scanline), static_cast<int16_t>(b.hPos));
      break;
    }
  }
  applied_ = list_;
  stale_ = false;
}

std::string Breakpoints::describeSwitch(const Breakpoint &b, const std::vector<SoftSwitchInfo> &switches) {
  const SoftSwitchInfo *sw = findSoftSwitch(switches, b.key.c_str());
  std::string name = sw ? sw->name : b.key;
  if (!sw) std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::toupper(c); });
  // On a machine without the switch the catalog cannot say whether it is a
  // register; the breakpoint can, since a one-bit switch is only ever 0 or 1
  // under the whole mask.
  const bool reg = sw ? sw->isRegister() : (b.mask != 0xFF || b.value > 1);
  char text[64];
  if (!b.equals) {
    std::snprintf(text, sizeof text, "%s changes", name.c_str());
  } else if (!reg) {
    std::snprintf(text, sizeof text, "%s %s", name.c_str(), b.value ? "on" : "off");
  } else if (b.mask == 0xFF) {
    std::snprintf(text, sizeof text, "%s = $%02X", name.c_str(), b.value);
  } else {
    std::snprintf(text, sizeof text, "%s & $%02X = $%02X", name.c_str(), b.mask, b.value);
  }
  return text;
}

void Breakpoints::writeSettings(std::string &out) const {
  for (const Breakpoint &b : list_) {
    char head[96];
    std::snprintf(head, sizeof head, "Breakpoint=%s\t%X\t%X\t%d\t", kindName(b.kind), b.start, b.end,
                  b.enabled ? 1 : 0);
    out += head + b.condition;
    char tail[96];
    if (b.kind == Breakpoint::Kind::Switch) {
      std::snprintf(tail, sizeof tail, "\t%s\t%u\t%u", b.equals ? "equals" : "change", b.value, b.mask);
      out += "\t" + b.key + tail;
    } else if (b.kind == Breakpoint::Kind::Beam) {
      std::snprintf(tail, sizeof tail, "\t%d\t%d\t%d", b.beamMode, b.scanline, b.hPos);
      out += tail;
    }
    out += "\n";
  }
}

namespace {
std::vector<std::string> splitTabs(const char *text) {
  std::vector<std::string> parts(1);
  for (const char *c = text; *c; c++) {
    if (*c == '\t') parts.emplace_back();
    else parts.back().push_back(*c);
  }
  return parts;
}
} // namespace

bool Breakpoints::readSetting(const char *line) {
  // The beam list's own line, from before it joined this one:
  // "Beam=<mode>\t<scanline>\t<hPos>\t<enabled>".
  if (!std::strncmp(line, "Beam=", 5)) {
    Breakpoint b;
    b.kind = Breakpoint::Kind::Beam;
    int enabled = 1;
    if (std::sscanf(line + 5, "%d\t%d\t%d\t%d", &b.beamMode, &b.scanline, &b.hPos, &enabled) == 4) {
      b.beamMode = std::clamp(b.beamMode, 0, 4);
      b.enabled = enabled;
      add(b);
    }
    return true;
  }
  // The Soft Switches window's: "SwitchBreakpoint=<key>\t<change|equals>\t<value>\t<mask>\t<enabled>".
  if (!std::strncmp(line, "SwitchBreakpoint=", 17)) {
    const std::vector<std::string> parts = splitTabs(line + 17);
    if (parts.size() < 5 || (parts[1] != "change" && parts[1] != "equals")) return true;
    Breakpoint b;
    b.kind = Breakpoint::Kind::Switch;
    b.key = parts[0];
    b.equals = parts[1] == "equals";
    b.value = static_cast<uint8_t>(std::strtoul(parts[2].c_str(), nullptr, 10));
    b.mask = static_cast<uint8_t>(std::strtoul(parts[3].c_str(), nullptr, 10));
    b.enabled = parts[4] == "1";
    add(b);
    return true;
  }
  if (std::strncmp(line, "Breakpoint=", 11) != 0) return false;
  const std::vector<std::string> parts = splitTabs(line + 11);
  if (parts.size() < 4) return true;
  Breakpoint b;
  bool known = false;
  for (int k = 0; k < KIND_COUNT; k++) {
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
  if (b.kind == Breakpoint::Kind::Switch) {
    if (parts.size() < 9 || parts[5].empty()) return true;
    b.key = parts[5];
    b.equals = parts[6] == "equals";
    b.value = static_cast<uint8_t>(std::strtoul(parts[7].c_str(), nullptr, 10));
    b.mask = static_cast<uint8_t>(std::strtoul(parts[8].c_str(), nullptr, 10));
  } else if (b.kind == Breakpoint::Kind::Beam) {
    if (parts.size() < 8) return true;
    b.beamMode = std::clamp(std::atoi(parts[5].c_str()), 0, 4);
    b.scanline = std::atoi(parts[6].c_str());
    b.hPos = std::atoi(parts[7].c_str());
  }
  add(b);
  return true;
}

} // namespace a2e::native
