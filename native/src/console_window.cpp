/*
 * console_window.cpp - The debugging console
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "console_window.hpp"

#include "cpu_debugger.hpp"
#include "emulation.hpp"
#include "ui_controls.hpp"
#include "ui_theme.hpp"

#include "debug/soft_switch_catalog.hpp"

#include "imgui.h"
#include "imgui_internal.h" // MarkIniSettingsDirty

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>

namespace a2e::native {

namespace {

using Kind = ConsoleCommand::Kind;
using host::CpuRegister;
using host::MachineHost;

// How much one command may print or touch, so a typing slip does not
// bury the console or hold the machine.
constexpr uint32_t MAX_DUMP = 0x1000;
constexpr int MAX_STEPS = 100000;
constexpr int MAX_FIND_RESULTS = 32;

// The processor's view of one bank: what the debugger reads and writes
// through, touching no switch.
host::MemorySpace processorSpace(uint32_t address) {
  host::MemorySpace space;
  space.kind = host::MemorySpace::Kind::Processor;
  space.base = address & 0xFF0000;
  space.size = 0x10000;
  return space;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::string upper(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
  return s;
}

// The flags a register line shows, a letter while it is set.
std::string flags(uint8_t p, bool native) {
  const char *names = native ? "NVMXDIZC" : "NV-BDIZC";
  std::string out;
  for (int i = 0; i < 8; i++) out += (p & (0x80 >> i)) ? names[i] : static_cast<char>(std::tolower(names[i]));
  return out;
}

} // namespace

ConsoleWindow::ConsoleWindow(Emulation &emulation, CpuDebugger &debugger, Breakpoints &breakpoints)
    : emulation_(emulation), debugger_(debugger), breakpoints_(breakpoints) {
  print("Type help for the commands. The monitor's own work too: 300.3FF, 300: A9 00, 300L, 300G.",
        Line::Style::Note);
}

void ConsoleWindow::setMachine(const MachineProfile &profile) {
  profile_ = &profile;
  wide_ = profile.cpu == CPUVariant::CMOS_65C816;
  emulation_.withMachine([&](MachineHost &host) { switches_ = host.softSwitches(); });
  seenStops_ = debugger_.stopCount();
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

void ConsoleWindow::print(const std::string &text, Line::Style style, std::optional<uint32_t> address) {
  output_.push_back({text, style, address});
  while (output_.size() > MAX_OUTPUT) output_.pop_front();
  scrollToEnd_ = true;
}

std::string ConsoleWindow::formatAddress(uint32_t address) const {
  char text[16];
  if (wide_) std::snprintf(text, sizeof text, "%02X/%04X", (address >> 16) & 0xFF, address & 0xFFFF);
  else std::snprintf(text, sizeof text, "%04X", address & 0xFFFF);
  return text;
}

// ---------------------------------------------------------------------------
// Reading what the user wrote
// ---------------------------------------------------------------------------

std::optional<uint32_t> ConsoleWindow::address(const std::string &text, uint32_t mask) {
  if (!mask) mask = addressMask();
  if (auto at = debugger_.symbols().resolve(text, mask)) return *at;
  // An expression, last: "PC+3", "DEEK($36)".
  if (auto v = value(text); v && *v >= 0 && static_cast<uint64_t>(*v) <= mask) return static_cast<uint32_t>(*v);
  return std::nullopt;
}

std::optional<int64_t> ConsoleWindow::value(const std::string &text) {
  if (auto at = debugger_.symbols().resolve(text, 0xFFFFFF)) return *at;
  int64_t result = 0;
  std::string problem;
  emulation_.withMachine([&](MachineHost &host) {
    result = host.evaluateExpression(text);
    problem = host.conditionError();
  });
  if (!problem.empty()) return std::nullopt;
  return result;
}

// ---------------------------------------------------------------------------
// Running a line
// ---------------------------------------------------------------------------

void ConsoleWindow::run(const std::string &line) {
  print("* " + line, Line::Style::Input);
  if (!line.empty() && (history_.empty() || history_.back() != line)) {
    history_.push_back(line);
    if (history_.size() > MAX_HISTORY) history_.erase(history_.begin());
    ImGui::MarkIniSettingsDirty();
  }
  historyAt_ = -1;
  const ConsoleCommand command = parseConsoleCommand(line);
  if (command.kind == Kind::Error) {
    error(command.error);
    return;
  }
  execute(command);
}

// The App applies the shared list once a frame, but a command that sets the
// machine running cannot wait for that: the machine runs on its own thread,
// and a breakpoint added one command earlier would be missed before the next
// frame applied it.
void ConsoleWindow::applyBreakpoints() {
  if (!breakpoints_.needsApply()) return;
  emulation_.withMachine([&](MachineHost &host) {
    if (MachineDebug *debug = host.debug()) breakpoints_.apply(*debug, host.softSwitches());
  });
}

void ConsoleWindow::execute(const ConsoleCommand &c) {
  switch (c.kind) {
  case Kind::Go:
  case Kind::Step:
  case Kind::StepOver:
  case Kind::StepOut:
  case Kind::Until: applyBreakpoints(); break;
  default: break;
  }
  switch (c.kind) {
  case Kind::Empty: return;
  case Kind::Error: error(c.error); return;
  case Kind::Help: help(c.text); return;
  case Kind::Clear: output_.clear(); return;

  case Kind::Dump: {
    const auto from = address(c.from);
    if (!from) return error("No address \"" + c.from + "\"");
    uint32_t to = 0;
    if (c.to.empty()) {
      to = *from + 7; // a line, as the monitor shows one
    } else {
      // The end is in the start's bank unless it says otherwise.
      const auto end = address(c.to);
      if (!end) return error("No address \"" + c.to + "\"");
      to = c.to.find('/') == std::string::npos && wide_ ? ((*from & 0xFF0000) | (*end & 0xFFFF)) : *end;
      if (to < *from) return error("A range runs upwards: 300.3FF");
    }
    if (to - *from >= MAX_DUMP) {
      print("Showing the first 4K", Line::Style::Note);
      to = *from + MAX_DUMP - 1;
    }
    dump(*from, to);
    return;
  }

  case Kind::Write:
  case Kind::Fill: {
    const auto from = address(c.from);
    if (!from) return error("No address \"" + c.from + "\"");
    std::vector<uint8_t> bytes;
    for (const std::string &v : c.values) {
      const auto b = value(v);
      if (!b || *b < 0 || *b > 0xFF) return error("Not a byte: \"" + v + "\"");
      bytes.push_back(static_cast<uint8_t>(*b));
    }
    uint32_t count = static_cast<uint32_t>(bytes.size());
    if (c.kind == Kind::Fill) {
      const auto to = address(c.to);
      if (!to || *to < *from) return error("Fill a range that runs upwards: f $2000.$3FFF 00");
      count = *to - *from + 1;
    }
    uint32_t refused = 0;
    emulation_.withMachine([&](MachineHost &host) {
      for (uint32_t i = 0; i < count; i++) {
        const uint32_t at = (*from + i) & addressMask();
        if (!host.pokeSpace(processorSpace(at), at, bytes[c.kind == Kind::Fill ? 0 : i])) refused++;
      }
    });
    if (refused) {
      print(std::to_string(refused) + " of " + std::to_string(count) + " not written: ROM or I/O",
            Line::Style::Error);
    } else if (c.kind == Kind::Fill) {
      print("Filled " + formatAddress(*from) + "." + formatAddress(*from + count - 1), Line::Style::Note);
    } else {
      dump(*from, *from + count - 1);
    }
    return;
  }

  case Kind::List: {
    std::optional<uint32_t> from;
    if (!c.from.empty()) {
      from = address(c.from);
      if (!from) return error("No address \"" + c.from + "\"");
    }
    list(from, c.count ? std::min(c.count, 200) : 20);
    return;
  }

  case Kind::Go: {
    std::optional<uint32_t> from;
    if (!c.from.empty()) {
      from = address(c.from);
      if (!from) return error("No address \"" + c.from + "\"");
    }
    emulation_.withMachine([&](MachineHost &host) {
      if (from) host.setRegister(CpuRegister::PC, *from);
      host.setPaused(false);
    });
    print(from ? "Running from " + formatAddress(*from) : "Running", Line::Style::Note);
    return;
  }

  case Kind::Step: {
    const int steps = std::clamp(c.count ? c.count : 1, 1, MAX_STEPS);
    ownStep_ = true;
    if (steps == 1) {
      debugger_.stepInto();
    } else {
      emulation_.withMachine([&](MachineHost &host) {
        host.setPaused(true);
        for (int i = 0; i < steps; i++) host.stepInstruction();
        host.forceRenderFrame();
      });
      debugger_.stepInto(); // the last, through the debugger so it follows
      if (steps > 1) print("Stepped " + std::to_string(steps), Line::Style::Note);
    }
    list(std::nullopt, 1);
    return;
  }
  case Kind::StepOver:
    debugger_.stepOver();
    print("Stepping over", Line::Style::Note);
    return;
  case Kind::StepOut:
    debugger_.stepOut();
    print("Running to the return", Line::Style::Note);
    return;
  case Kind::Pause:
    emulation_.withMachine([](MachineHost &host) { host.setPaused(true); });
    return;
  case Kind::Until: {
    const auto to = address(c.from);
    if (!to) return error("No address \"" + c.from + "\"");
    debugger_.runTo(*to);
    print("Running to " + formatAddress(*to), Line::Style::Note);
    return;
  }

  case Kind::Registers: {
    for (const auto &[name, text] : c.assignments) {
      const auto v = name == "pc" ? std::optional<int64_t>(address(text)) : value(text);
      if (!v) return error("Not a value: \"" + text + "\"");
      CpuRegister reg = CpuRegister::A;
      if (name == "a") reg = CpuRegister::A;
      else if (name == "x") reg = CpuRegister::X;
      else if (name == "y") reg = CpuRegister::Y;
      else if (name == "sp" || name == "s") reg = CpuRegister::SP;
      else if (name == "pc") reg = CpuRegister::PC;
      else if (name == "p") reg = CpuRegister::P;
      else if (name == "pbr" || name == "k") reg = CpuRegister::PBR;
      else if (name == "dbr" || name == "b") reg = CpuRegister::DBR;
      else reg = CpuRegister::D;
      if (!wide_ && (reg == CpuRegister::PBR || reg == CpuRegister::DBR || reg == CpuRegister::D)) {
        return error("A 6502 has no " + upper(name));
      }
      emulation_.withMachine([&](MachineHost &host) { host.setRegister(reg, static_cast<uint32_t>(*v)); });
    }
    registers();
    return;
  }

  case Kind::Evaluate: {
    int64_t result = 0;
    std::string problem;
    emulation_.withMachine([&](MachineHost &host) {
      result = host.evaluateExpression(c.text);
      problem = host.conditionError();
    });
    if (!problem.empty()) return error(problem);
    char text[64];
    const uint32_t bits = static_cast<uint32_t>(result);
    std::snprintf(text, sizeof text, bits > 0xFFFF ? "= $%06X  %lld" : bits > 0xFF ? "= $%04X  %lld" : "= $%02X  %lld",
                  bits & 0xFFFFFF, static_cast<long long>(result));
    print(text);
    return;
  }

  case Kind::BreakAdd: breakpoint(c); return;
  case Kind::BreakList: listBreakpoints(); return;
  case Kind::BreakDelete:
  case Kind::BreakEnable:
  case Kind::BreakDisable: {
    auto &all = breakpoints_.all();
    if (c.all) {
      if (c.kind == Kind::BreakDelete) all.clear();
      for (Breakpoint &b : all) b.enabled = c.kind == Kind::BreakEnable;
    } else {
      if (c.count < 1 || static_cast<size_t>(c.count) > all.size()) {
        return error("No breakpoint " + std::to_string(c.count) + "; bl lists them");
      }
      const size_t i = static_cast<size_t>(c.count - 1);
      if (c.kind == Kind::BreakDelete) breakpoints_.remove(i);
      else all[i].enabled = c.kind == Kind::BreakEnable;
    }
    applyBreakpoints();
    ImGui::MarkIniSettingsDirty();
    listBreakpoints();
    return;
  }

  case Kind::Symbol: {
    const DebugSymbols &symbols = debugger_.symbols();
    // A name first, then an address, as resolve reads them.
    const std::string text = c.text;
    const bool number = text[0] == '$' || text.find('/') != std::string::npos;
    if (!number) {
      if (auto at = symbols.resolve(text, addressMask()); at && symbols.lookup(*at) &&
                                                            lower(symbols.lookup(*at)->name) == lower(text)) {
        const auto sym = symbols.lookup(*at);
        print(sym->name + " = " + formatAddress(*at) + (sym->description.empty() ? "" : "  " + sym->description),
              Line::Style::Output, *at);
        return;
      }
    }
    const auto at = address(text);
    if (!at) return error("No name or address \"" + text + "\"");
    if (const auto sym = symbols.lookup(*at)) {
      print(formatAddress(*at) + " is " + sym->name + (sym->description.empty() ? "" : "  " + sym->description),
            Line::Style::Output, *at);
    } else {
      print(formatAddress(*at) + " has no name", Line::Style::Note, *at);
    }
    return;
  }

  case Kind::Stack: {
    host::CpuState cpu;
    std::vector<std::pair<uint32_t, uint8_t>> bytes;
    emulation_.withMachine([&](MachineHost &host) {
      cpu = host.cpuState();
      // From the top of the stack to its bottom, at most a page of it.
      const bool page1 = !wide_ || cpu.emulation();
      const uint32_t sp = page1 ? (0x100 | (cpu.sp & 0xFF)) : cpu.sp;
      const uint32_t top = page1 ? 0x1FF : std::min<uint32_t>(sp + 0x100, 0xFFFF);
      for (uint32_t a = sp + 1; a <= top && bytes.size() < 64; a++) bytes.emplace_back(a, host.peek(a));
    });
    if (bytes.empty()) return print("The stack is empty", Line::Style::Note);
    for (size_t i = 0; i < bytes.size(); i++) {
      char text[48];
      std::snprintf(text, sizeof text, "%04X  %02X", bytes[i].first, bytes[i].second);
      std::string line = text;
      // A pair that is a JSR's return address, less one.
      if (i + 1 < bytes.size()) {
        const uint32_t ret = (bytes[i].second | (bytes[i + 1].second << 8)) + 1;
        if (const auto sym = debugger_.symbols().lookup(ret & 0xFFFF)) line += "    RTS to " + sym->name;
      }
      print(line);
    }
    return;
  }

  case Kind::Trace: {
    std::vector<host::TraceLine> lines;
    bool recording = false;
    emulation_.withMachine([&](MachineHost &host) {
      MachineDebug *debug = host.debug();
      if (!debug) return;
      recording = debug->isTraceEnabled();
      const size_t have = debug->traceCount();
      const size_t want = std::min<size_t>(c.count ? static_cast<size_t>(c.count) : 20, have);
      lines = host.traceLines(have - want, want);
    });
    if (!recording && lines.empty()) {
      return print("Nothing recorded: turn on Record in the CPU debugger's Trace tab", Line::Style::Note);
    }
    for (const host::TraceLine &t : lines) {
      char regs[64];
      std::snprintf(regs, sizeof regs, "  A=%02X X=%02X Y=%02X SP=%02X", t.a & 0xFF, t.x & 0xFF, t.y & 0xFF,
                    t.sp & 0xFF);
      std::string text = formatAddress(t.instruction.address) + "  " + t.instruction.mnemonic;
      if (!t.instruction.operand.empty()) text += " " + t.instruction.operand;
      while (text.size() < 28) text += ' ';
      print(text + regs, Line::Style::Output, t.instruction.address);
    }
    return;
  }

  case Kind::Find: find(c); return;

  case Kind::Reset:
    emulation_.withMachine([](MachineHost &host) { host.warmReset(); });
    print("Control-Reset", Line::Style::Note);
    return;
  case Kind::Reboot:
    emulation_.withMachine([](MachineHost &host) { host.reset(); });
    print("Rebooted", Line::Style::Note);
    return;
  }
}

// ---------------------------------------------------------------------------
// Commands that print a lot
// ---------------------------------------------------------------------------

void ConsoleWindow::dump(uint32_t from, uint32_t to) {
  std::vector<uint8_t> bytes(to - from + 1);
  emulation_.withMachine([&](MachineHost &host) {
    for (uint32_t i = 0; i < bytes.size(); i++) bytes[i] = host.peekSpace(processorSpace(from + i), from + i);
  });
  // As the monitor shows them: the first line from the address asked for,
  // then eight to a line on the eights, with the text the bytes spell beside
  // them, high bit or not.
  uint32_t row = from;
  while (row <= to) {
    const uint32_t end = std::min(to, row | 7u);
    std::string hex, text;
    for (uint32_t a = row; a <= end; a++) {
      const uint8_t b = bytes[a - from];
      char cell[4];
      std::snprintf(cell, sizeof cell, " %02X", b);
      hex += cell;
      const char ch = static_cast<char>(b & 0x7F);
      text += ch >= 0x20 && ch < 0x7F ? ch : '.';
    }
    while (hex.size() < 24) hex += ' ';
    print(formatAddress(row) + "-" + hex + "   " + text, Line::Style::Output, row);
    if (end == 0xFFFFFFFFu || end + 1 < row) break; // the top of the address space
    row = end + 1;
  }
}

void ConsoleWindow::printInstruction(const host::Instruction &in, bool atPC) {
  char bytes[16] = "";
  for (int i = 0; i < in.length && i < 4; i++) {
    char b[4];
    std::snprintf(b, sizeof b, "%02X ", in.bytes[i]);
    std::strcat(bytes, b);
  }
  std::string text = formatAddress(in.address) + (atPC ? " > " : "   ") + bytes;
  while (text.size() < (wide_ ? 24u : 22u)) text += ' ';
  text += in.mnemonic;
  if (!in.operand.empty()) text += " " + in.operand;
  // Where a jump, branch or call goes, or the pointer an indirect jump goes
  // through, by name; a BRK or an RTS goes nowhere the listing knows.
  const bool goes = in.flow == a2e::FlowType::CALL || in.flow == a2e::FlowType::UNCONDITIONAL ||
                    in.flow == a2e::FlowType::CONDITIONAL || in.flow == a2e::FlowType::INDIRECT;
  if (const auto sym = debugger_.symbols().lookup(in.target); sym && goes && in.target != in.address) {
    while (text.size() < 40) text += ' ';
    text += "; " + sym->name;
  } else if (const auto here = debugger_.symbols().lookup(in.address)) {
    while (text.size() < 40) text += ' ';
    text += "; " + here->name + ":";
  }
  print(text, Line::Style::Output, in.address);
}

void ConsoleWindow::list(std::optional<uint32_t> from, int count) {
  std::vector<host::Instruction> lines;
  uint32_t pc = 0;
  emulation_.withMachine([&](MachineHost &host) {
    pc = host.cpuState().pc;
    lines = host.disassembleRange(from.value_or(pc), 0, count);
  });
  for (const host::Instruction &in : lines) printInstruction(in, in.address == pc);
}

void ConsoleWindow::registers() {
  host::CpuState c;
  emulation_.withMachine([&](MachineHost &host) { c = host.cpuState(); });
  char text[160];
  if (wide_) {
    std::snprintf(text, sizeof text, "PC=%02X/%04X A=%04X X=%04X Y=%04X SP=%04X D=%04X DBR=%02X P=%02X %s%s",
                  c.pbr, c.pc & 0xFFFF, c.a, c.x, c.y, c.sp, c.d, c.dbr, c.p,
                  flags(c.p, !c.emulation()).c_str(), c.emulation() ? " E" : "");
  } else {
    std::snprintf(text, sizeof text, "PC=%04X A=%02X X=%02X Y=%02X SP=%02X P=%02X %s", c.pc & 0xFFFF, c.a & 0xFF,
                  c.x & 0xFF, c.y & 0xFF, c.sp & 0xFF, c.p, flags(c.p, false).c_str());
  }
  print(text, Line::Style::Output, c.pc);
}

std::string ConsoleWindow::describe(const Breakpoint &b) const {
  std::string text;
  switch (b.kind) {
  case Breakpoint::Kind::Exec: text = "exec  "; break;
  case Breakpoint::Kind::Read: text = "read  "; break;
  case Breakpoint::Kind::Write: text = "write "; break;
  case Breakpoint::Kind::ReadWrite: text = "r/w   "; break;
  case Breakpoint::Kind::Stack: text = "sp    "; break;
  case Breakpoint::Kind::Switch: text = "switch "; break;
  case Breakpoint::Kind::Beam: text = "beam  "; break;
  }
  char where[64];
  if (b.kind == Breakpoint::Kind::Switch) {
    text += Breakpoints::describeSwitch(b, switches_);
  } else if (b.kind == Breakpoint::Kind::Beam) {
    const int column = profile_ && b.hPos >= 0 ? b.hPos - profile_->timing.hblankCycles : b.hPos;
    switch (b.beamMode) {
    case Breakpoint::BeamVbl: std::snprintf(where, sizeof where, "vertical blank (line %d)", b.scanline); break;
    case Breakpoint::BeamHbl: std::snprintf(where, sizeof where, "horizontal blank, every line"); break;
    case Breakpoint::BeamLine: std::snprintf(where, sizeof where, "line %d", b.scanline); break;
    case Breakpoint::BeamColumn: std::snprintf(where, sizeof where, "column %d, every line", column); break;
    default: std::snprintf(where, sizeof where, "line %d, column %d", b.scanline, column); break;
    }
    text += where;
  } else if (b.kind == Breakpoint::Kind::Stack) {
    std::snprintf(where, sizeof where, b.isRange() ? "SP $%X-$%X" : "SP $%X", b.start, b.end);
    text += where;
  } else {
    text += formatAddress(b.start);
    if (b.isRange()) text += "-" + formatAddress(b.end);
    if (const auto sym = debugger_.symbols().lookup(b.start)) text += " " + sym->name;
  }
  if (!b.condition.empty()) text += "  if " + b.condition;
  return text;
}

void ConsoleWindow::breakpoint(const ConsoleCommand &c) {
  Breakpoint b = c.breakpoint;
  switch (b.kind) {
  case Breakpoint::Kind::Switch:
    if (!findSoftSwitch(switches_, b.key.c_str())) {
      print("No switch \"" + b.key + "\" on this machine; kept for one that has it", Line::Style::Note);
    }
    break;
  case Breakpoint::Kind::Beam: {
    if (!profile_) return;
    const MachineTiming &t = profile_->timing;
    // The parser's column is a visible column; the core counts blanking too.
    switch (b.beamMode) {
    case Breakpoint::BeamVbl: b.scanline = t.visibleScanlines; b.hPos = 0; break;
    case Breakpoint::BeamHbl: b.scanline = -1; b.hPos = 0; break;
    case Breakpoint::BeamLine: b.hPos = -1; break;
    case Breakpoint::BeamColumn: b.scanline = -1; b.hPos += t.hblankCycles; break;
    default: b.hPos += t.hblankCycles; break;
    }
    if (b.scanline >= t.scanlinesPerFrame) return error("Line 0 to " + std::to_string(t.scanlinesPerFrame - 1));
    if (b.hPos >= t.cyclesPerScanline) return error("Column 0 to " + std::to_string(t.visibleColumns - 1));
    int beams = 0;
    for (const Breakpoint &x : breakpoints_.all()) beams += x.kind == Breakpoint::Kind::Beam;
    if (beams >= static_cast<int>(MachineDebug::MAX_BEAM_BREAKPOINTS)) return error("No room for another beam breakpoint");
    break;
  }
  default: {
    const uint32_t mask = b.kind == Breakpoint::Kind::Stack ? (wide_ ? 0xFFFF : 0xFF) : addressMask();
    const auto from = address(c.from, mask);
    if (!from) return error("No address \"" + c.from + "\"");
    uint32_t to = *from;
    if (!c.to.empty()) {
      const auto end = address(c.to, mask);
      if (!end || *end < *from) return error("No end \"" + c.to + "\", or it is below the start");
      to = *end;
    }
    b.start = *from;
    b.end = to;
    break;
  }
  }
  if (!breakpoints_.add(b)) return error("That breakpoint is already set");
  applyBreakpoints();
  ImGui::MarkIniSettingsDirty();
  printBreakpoint(breakpoints_.all().size() - 1);
}

// One breakpoint, numbered as bd, be and bx take it, a dash if it is off.
void ConsoleWindow::printBreakpoint(size_t index) {
  const Breakpoint &b = breakpoints_.all()[index];
  char head[24];
  std::snprintf(head, sizeof head, "%-3zu%s ", index + 1, b.enabled ? " " : "-");
  std::string text = head + describe(b);
  if (b.hits) text += "  (" + std::to_string(b.hits) + (b.hits == 1 ? " hit)" : " hits)");
  print(text, b.enabled ? Line::Style::Output : Line::Style::Note,
        b.isAddress() ? std::optional<uint32_t>(b.start) : std::nullopt);
}

void ConsoleWindow::listBreakpoints() {
  const auto &all = breakpoints_.all();
  if (all.empty()) return print("No breakpoints", Line::Style::Note);
  for (size_t i = 0; i < all.size(); i++) printBreakpoint(i);
}

void ConsoleWindow::find(const ConsoleCommand &c) {
  // What to look for: bytes with ?? for any, or the text, matched with the
  // high bit set or clear, as the Apple II stores it either way.
  std::vector<int> pattern;
  if (!c.text.empty()) {
    for (char ch : c.text) pattern.push_back(static_cast<unsigned char>(ch) & 0x7F);
  } else {
    for (const std::string &v : c.values) {
      if (v == "??") {
        pattern.push_back(-1);
        continue;
      }
      const auto b = value(v);
      if (!b || *b < 0 || *b > 0xFF) return error("Not a byte: \"" + v + "\"");
      pattern.push_back(static_cast<int>(*b));
    }
  }
  if (pattern.empty()) return;
  const bool text = !c.text.empty();

  // The bank the PC is in, which on anything but a IIgs is all of it.
  std::vector<uint8_t> memory(0x10000);
  uint32_t bank = 0;
  emulation_.withMachine([&](MachineHost &host) {
    bank = wide_ ? host.cpuState().pc & 0xFF0000 : 0;
    host.readSpace(processorSpace(bank), bank, memory.data(), memory.size());
  });
  int found = 0;
  for (size_t a = 0; a + pattern.size() <= memory.size(); a++) {
    bool match = true;
    for (size_t i = 0; i < pattern.size() && match; i++) {
      const int want = pattern[i];
      const int got = text ? memory[a + i] & 0x7F : memory[a + i];
      match = want < 0 || want == got;
    }
    if (!match) continue;
    if (++found > MAX_FIND_RESULTS) {
      print("And more; showing the first " + std::to_string(MAX_FIND_RESULTS), Line::Style::Note);
      break;
    }
    const uint32_t at = bank | static_cast<uint32_t>(a);
    std::string where = formatAddress(at);
    if (const auto sym = debugger_.symbols().lookup(at)) where += "  " + sym->name;
    print(where, Line::Style::Output, at);
  }
  if (!found) print("Not found" + std::string(wide_ ? " in this bank" : ""), Line::Style::Note);
}

void ConsoleWindow::help(const std::string &topic) {
  const std::string want = lower(topic);
  bool shown = false;
  for (const ConsoleCommandHelp &h : consoleCommands()) {
    std::string names = h.name;
    if (!want.empty()) {
      bool match = want == h.name;
      std::string aliases = h.aliases;
      size_t at = 0;
      while (!match && at < aliases.size()) {
        const size_t space = aliases.find(' ', at);
        match = want == aliases.substr(at, space == std::string::npos ? std::string::npos : space - at);
        at = space == std::string::npos ? aliases.size() : space + 1;
      }
      // "help bp" covers bp sw and bp beam too.
      match = match || std::string(h.name).rfind(want + " ", 0) == 0;
      if (!match) continue;
    }
    std::string usage = h.usage;
    while (usage.size() < 36) usage += ' ';
    print(usage + "  " + h.summary);
    if (!want.empty() && *h.aliases) print(std::string("    also: ") + h.aliases, Line::Style::Note);
    shown = true;
  }
  if (!shown) error("No command \"" + topic + "\"");
  if (want.empty()) {
    print("Monitor: 300 300.3FF 300: A9 00 300L 300G, and E1/2000 on a IIgs", Line::Style::Note);
    print("Addresses are $hex or names (COUT, HOME); values may be expressions (PEEK($24)+1)", Line::Style::Note);
  }
}

// ---------------------------------------------------------------------------
// Each frame
// ---------------------------------------------------------------------------

// A stop the debugger has judged, said here in its words. One this console
// caused by stepping it has said itself.
void ConsoleWindow::update() {
  const uint32_t stops = debugger_.stopCount();
  if (stops == seenStops_) return;
  seenStops_ = stops;
  if (ownStep_) {
    ownStep_ = false;
    return;
  }
  const std::string &reason = debugger_.stopReason();
  if (reason == "Stepped") return;
  print("Stopped: " + reason, Line::Style::Stop);
  list(std::nullopt, 1);
}

namespace {

// The history on Up and Down, and Tab finishing a command's name or a
// symbol's.
struct InputState {
  ConsoleWindow *window;
  std::vector<std::string> *history;
  int *historyAt;
  std::string *typed;
  const DebugSymbols *symbols;
  std::vector<std::string> *suggestions;
};

int inputCallback(ImGuiInputTextCallbackData *data) {
  auto *s = static_cast<InputState *>(data->UserData);
  if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
    if (s->history->empty()) return 0;
    int &at = *s->historyAt;
    const int size = static_cast<int>(s->history->size());
    if (at < 0) *s->typed = std::string(data->Buf, data->BufTextLen);
    if (data->EventKey == ImGuiKey_UpArrow) at = at < 0 ? size - 1 : std::max(0, at - 1);
    else if (at >= 0) at = at + 1 >= size ? -1 : at + 1;
    const std::string &line = at < 0 ? *s->typed : (*s->history)[static_cast<size_t>(at)];
    data->DeleteChars(0, data->BufTextLen);
    data->InsertChars(0, line.c_str());
    return 0;
  }
  if (data->EventFlag == ImGuiInputTextFlags_CallbackCompletion) {
    // The word under the cursor, back to the last space.
    int start = data->CursorPos;
    while (start > 0 && data->Buf[start - 1] != ' ') start--;
    const std::string word = lower(std::string(data->Buf + start, data->CursorPos - start));
    if (word.empty()) return 0;
    std::vector<std::string> candidates;
    if (start == 0) {
      for (const ConsoleCommandHelp &h : consoleCommands()) {
        const std::string name = h.name;
        if (name.find(' ') == std::string::npos && name.rfind(word, 0) == 0) candidates.push_back(name);
      }
    } else {
      for (const auto &[address, label] : s->symbols->labels()) {
        if (!label.name.empty() && lower(label.name).rfind(word, 0) == 0) candidates.push_back(label.name);
      }
      for (const std::string &name : s->symbols->builtInNames()) {
        if (lower(name).rfind(word, 0) == 0) candidates.push_back(name);
      }
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    if (candidates.empty()) return 0;
    // As far as every candidate agrees; the rest are listed.
    std::string common = candidates[0];
    for (const std::string &c : candidates) {
      size_t n = 0;
      while (n < common.size() && n < c.size() && std::tolower(common[n]) == std::tolower(c[n])) n++;
      common.resize(n);
    }
    if (common.size() > word.size() || candidates.size() == 1) {
      data->DeleteChars(start, data->CursorPos - start);
      data->InsertChars(data->CursorPos, (candidates.size() == 1 ? candidates[0] + " " : common).c_str());
    }
    if (candidates.size() > 1) *s->suggestions = candidates;
  }
  return 0;
}

} // namespace

void ConsoleWindow::draw(bool *open) {
  if (!open || !*open) return;
  ui::BeforeWindow("Console");
  ImGui::SetNextWindowSize(ImVec2(760, 460), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(360, 200), ImVec2(FLT_MAX, FLT_MAX));
  if (!ui::BeginWindow("Console", open)) {
    ImGui::End();
    return;
  }
  const ui::Palette &p = ui::palette();
  const float inputHeight = ImGui::GetFrameHeightWithSpacing() + 6;

  ImGui::BeginChild("##output", ImVec2(0, -inputHeight), ImGuiChildFlags_None,
                    ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PushFont(ui::monoFont(), 0.0f);
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(output_.size()));
  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
      const Line &line = output_[static_cast<size_t>(i)];
      ImU32 colour = ImGui::GetColorU32(ImGuiCol_Text);
      switch (line.style) {
      case Line::Style::Input: colour = ui::accentText(); break;
      case Line::Style::Error: colour = p.red; break;
      case Line::Style::Note: colour = ImGui::GetColorU32(ImGuiCol_Text, 0.62f); break;
      case Line::Style::Stop: colour = p.orange; break;
      case Line::Style::Output: break;
      }
      ImGui::PushStyleColor(ImGuiCol_Text, colour);
      ImGui::TextUnformatted(line.text.c_str());
      ImGui::PopStyleColor();
      if (line.address && ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, b.y), ImVec2(b.x, b.y), colour);
        ImGui::SetTooltip("Show %s in the CPU debugger", formatAddress(*line.address).c_str());
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && showAddress) showAddress(*line.address);
      }
    }
  }
  ImGui::PopFont();
  if (scrollToEnd_) {
    ImGui::SetScrollHereY(1.0f);
    scrollToEnd_ = false;
  }
  ImGui::EndChild();

  // The prompt: the monitor's asterisk, and the line being typed.
  ImGui::Dummy(ImVec2(0, 2));
  ImGui::PushFont(ui::monoFont(), 0.0f);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::accentText()), "*");
  ImGui::SameLine();
  std::vector<std::string> suggestions;
  InputState state{this, &history_, &historyAt_, &typed_, &debugger_.symbols(), &suggestions};
  ImGui::SetNextItemWidth(-1);
  if (focusInput_) {
    ImGui::SetKeyboardFocusHere();
    focusInput_ = false;
  }
  const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory |
                                    ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_EscapeClearsAll;
  if (ImGui::InputTextWithHint("##input", "help, 300.3FF, bp COUT, s, r, ? PEEK($24)", input_.data(), input_.size(),
                               flags, inputCallback, &state)) {
    const std::string line = input_.data();
    input_[0] = 0;
    run(line);
    focusInput_ = true;
  }
  ImGui::PopFont();
  if (!suggestions.empty()) {
    std::string all;
    for (size_t i = 0; i < suggestions.size() && i < 40; i++) all += (i ? "  " : "") + suggestions[i];
    if (suggestions.size() > 40) all += "  ...";
    print(all, Line::Style::Note);
  }
  ImGui::End();
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void ConsoleWindow::writeSettings(std::string &out) const {
  // The most recent hundred, which is plenty to come back to.
  const size_t from = history_.size() > 100 ? history_.size() - 100 : 0;
  for (size_t i = from; i < history_.size(); i++) out += "History=" + history_[i] + "\n";
}

void ConsoleWindow::readSetting(const char *line) {
  if (!std::strncmp(line, "History=", 8) && line[8]) history_.emplace_back(line + 8);
}

} // namespace a2e::native
