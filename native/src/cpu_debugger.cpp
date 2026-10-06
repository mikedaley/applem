/*
 * cpu_debugger.cpp - The CPU debugger window
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "cpu_debugger.hpp"

#include "emulation.hpp"
#include "media_store.hpp"
#include "rule_builder.hpp"
#include "ui_controls.hpp"
#include "ui_theme.hpp"

#include "imgui_internal.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace a2e::native {

namespace {

constexpr float SIDEBAR_WIDTH = 300.0f;
constexpr float CARD_ROUNDING = 10.0f;
constexpr float PAD = 12.0f;
constexpr float PANEL_MIN = 120.0f;
constexpr float CODE_MIN = 160.0f;
constexpr float PC_HEIGHT = 42.0f;
// Lines decoded beyond each edge of the listing, for a scroll part of the
// way to the next line to show.
constexpr int LINES_ABOVE = 4;
constexpr int LINES_BELOW = 6;
// The longest selection that is totalled, in instructions.
constexpr int SELECTION_MAX = 2048;
// Branch arrows: how many lanes, and how far apart.
constexpr int ARROW_LANES = 6;
constexpr float ARROW_PITCH = 6.0f;
// An arrow leaves its lane on a slant and runs that far, at least, before
// its head: the gap between the innermost lane and the head, and the slope
// (down per across) of the approach.
constexpr float ARROW_APPROACH = 6.0f;
constexpr float ARROW_SLOPE = 0.6f;
constexpr float ARROW_HEAD_LENGTH = 6.0f;
constexpr float ARROW_HEAD_HALF = 3.5f;
constexpr float SCROLLBAR_WIDTH = 10.0f;
// Instructions shown above the PC when the listing is centred on it.
constexpr int CONTEXT_ABOVE = 6;
// The stack as far as it goes: all of page one above SP on a 6502 or in
// emulation mode, and this many entries of a native stack, which has no
// page to end at. The card scrolls through them.
constexpr int STACK_ROWS = 256;
constexpr int TRACE_ROWS_MAX = 100000;

// The Apple logo's six stripes, which every colour in the app comes from,
// made readable for the current appearance by the theme.
using ui::Palette;
using ui::palette;

ImU32 text(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_Text, alpha); }
ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }
ImU32 accent(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_CheckMark, alpha); }

ImU32 withAlpha(ImU32 colour, float alpha) {
  const int a = static_cast<int>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

ImU32 cardFill() { return ui::isDark() ? IM_COL32(255, 255, 255, 10) : IM_COL32(0, 0, 0, 7); }
ImU32 well() { return ui::isDark() ? IM_COL32(0, 0, 0, 80) : IM_COL32(255, 255, 255, 200); }

void card(ImDrawList *draw, ImVec2 a, ImVec2 b) {
  draw->AddRectFilled(a, b, cardFill(), CARD_ROUNDING);
  draw->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), CARD_ROUNDING);
}

// Small capitals, as the Joystick and Mockingboard windows label things.
void caption(ImDrawList *draw, ImVec2 at, const char *label, ImU32 colour) {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  draw->AddText(at, colour, label);
  ImGui::PopFont();
}

float captionHeight() { return ImGui::GetFontSize() * ui::SMALL_TEXT; }

// A capsule with a word in it. Returns its width.
float pill(ImDrawList *draw, ImVec2 at, const char *label, bool lit, ImU32 colour, float scale = ui::SMALL_TEXT) {
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * scale);
  const ImVec2 size = ImGui::CalcTextSize(label);
  const ImVec2 end(at.x + size.x + 12, at.y + size.y + 4);
  draw->AddRectFilled(at, end, lit ? colour : text(0.07f), (end.y - at.y) * 0.5f);
  draw->AddText(ImVec2(at.x + 6, at.y + 2), lit ? ui::textOn(colour) : secondary(), label);
  ImGui::PopFont();
  return end.x - at.x;
}

std::string grouped(uint64_t value) {
  std::string digits = std::to_string(value);
  std::string out;
  int count = 0;
  for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
    if (count && count % 3 == 0) out.insert(out.begin(), ',');
    out.insert(out.begin(), *it);
    count++;
  }
  return out;
}

ImU32 categoryColour(InstrCategory category, const Palette &p) {
  switch (category) {
  case InstrCategory::BRANCH: return p.purple;
  case InstrCategory::LOAD: return p.blue;
  case InstrCategory::MATH: return p.green;
  case InstrCategory::STACK: return p.yellow;
  case InstrCategory::FLAG: return p.orange;
  case InstrCategory::UNKNOWN: return p.red;
  }
  return text();
}

ImU32 symbolColour(DebugSymbols::Category category, const Palette &p) {
  switch (category) {
  case DebugSymbols::Category::ZeroPage: return p.green;
  case DebugSymbols::Category::SoftSwitch: return p.orange;
  case DebugSymbols::Category::Disk: return p.purple;
  case DebugSymbols::Category::Rom: return p.blue;
  case DebugSymbols::Category::Basic: return p.yellow;
  case DebugSymbols::Category::Vector: return p.red;
  case DebugSymbols::Category::Io: return p.orange;
  case DebugSymbols::Category::Imported:
  case DebugSymbols::Category::User: return accent();
  }
  return text();
}

// The toolbar's icons, drawn rather than taken from a font, so they are the
// same at any size and in either appearance.
enum class Icon { Play, Pause, StepInto, StepOver, StepOut, Follow, Back, Forward };

void drawIcon(ImDrawList *draw, Icon icon, ImVec2 c, float s, ImU32 colour) {
  const float t = std::max(1.5f, s * 0.13f);
  switch (icon) {
  case Icon::Play:
    draw->AddTriangleFilled(ImVec2(c.x - s * 0.32f, c.y - s * 0.42f), ImVec2(c.x - s * 0.32f, c.y + s * 0.42f),
                            ImVec2(c.x + s * 0.42f, c.y), colour);
    break;
  case Icon::Pause:
    draw->AddRectFilled(ImVec2(c.x - s * 0.36f, c.y - s * 0.4f), ImVec2(c.x - s * 0.1f, c.y + s * 0.4f), colour, 1.5f);
    draw->AddRectFilled(ImVec2(c.x + s * 0.1f, c.y - s * 0.4f), ImVec2(c.x + s * 0.36f, c.y + s * 0.4f), colour, 1.5f);
    break;
  case Icon::StepInto:
    draw->AddLine(ImVec2(c.x, c.y - s * 0.45f), ImVec2(c.x, c.y + s * 0.15f), colour, t);
    draw->AddTriangleFilled(ImVec2(c.x - s * 0.28f, c.y + s * 0.02f), ImVec2(c.x + s * 0.28f, c.y + s * 0.02f),
                            ImVec2(c.x, c.y + s * 0.32f), colour);
    draw->AddCircleFilled(ImVec2(c.x, c.y + s * 0.45f), s * 0.09f, colour);
    break;
  case Icon::StepOver:
    draw->PathArcTo(ImVec2(c.x, c.y + s * 0.05f), s * 0.36f, IM_PI * 1.05f, IM_PI * 1.9f, 12);
    draw->PathStroke(colour, 0, t);
    draw->AddTriangleFilled(ImVec2(c.x + s * 0.18f, c.y - s * 0.08f), ImVec2(c.x + s * 0.48f, c.y - s * 0.2f),
                            ImVec2(c.x + s * 0.4f, c.y + s * 0.12f), colour);
    draw->AddCircleFilled(ImVec2(c.x, c.y + s * 0.42f), s * 0.09f, colour);
    break;
  case Icon::StepOut:
    draw->AddLine(ImVec2(c.x, c.y + s * 0.3f), ImVec2(c.x, c.y - s * 0.2f), colour, t);
    draw->AddTriangleFilled(ImVec2(c.x - s * 0.28f, c.y - s * 0.08f), ImVec2(c.x + s * 0.28f, c.y - s * 0.08f),
                            ImVec2(c.x, c.y - s * 0.4f), colour);
    draw->AddCircleFilled(ImVec2(c.x, c.y + s * 0.45f), s * 0.09f, colour);
    break;
  case Icon::Back:
  case Icon::Forward: {
    const float d = icon == Icon::Back ? -1.0f : 1.0f;
    draw->AddLine(ImVec2(c.x - d * s * 0.15f, c.y - s * 0.35f), ImVec2(c.x + d * s * 0.2f, c.y), colour, t);
    draw->AddLine(ImVec2(c.x + d * s * 0.2f, c.y), ImVec2(c.x - d * s * 0.15f, c.y + s * 0.35f), colour, t);
    break;
  }
  case Icon::Follow:
    draw->AddCircle(c, s * 0.34f, colour, 0, t);
    draw->AddCircleFilled(c, s * 0.12f, colour);
    for (int i = 0; i < 4; i++) {
      const float a = i * IM_PI * 0.5f;
      draw->AddLine(ImVec2(c.x + std::cos(a) * s * 0.34f, c.y + std::sin(a) * s * 0.34f),
                    ImVec2(c.x + std::cos(a) * s * 0.5f, c.y + std::sin(a) * s * 0.5f), colour, t);
    }
    break;
  }
}

// A toolbar button: an icon and a word, highlighted when hovered and filled
// with the accent when it is the one to press. Compact, it is a frame's
// height, to sit in a row of fields and buttons.
bool toolButton(const char *id, Icon icon, const char *label, const char *tip, bool enabled = true,
                bool primary = false, bool compact = false) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float height = ImGui::GetFrameHeight() + (compact ? 0 : 6);
  const float rounding = compact ? ImGui::GetStyle().FrameRounding : 7.0f;
  const float textWidth = label ? ImGui::CalcTextSize(label).x : 0;
  const float width = height + (label ? textWidth + 6 : 0) + (label ? 8 : 0);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::BeginDisabled(!enabled);
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(width, height));
  const bool hovered = ImGui::IsItemHovered();
  const bool held = ImGui::IsItemActive();
  ImGui::EndDisabled();
  const ImVec2 end(at.x + width, at.y + height);
  if (primary && enabled) {
    draw->AddRectFilled(at, end, accent(held ? 0.75f : hovered ? 0.9f : 1.0f), rounding);
  } else if (enabled && (hovered || held)) {
    draw->AddRectFilled(at, end, text(held ? 0.14f : 0.08f), rounding);
  }
  const ImU32 colour = !enabled ? text(0.25f) : primary ? IM_COL32_WHITE : text(0.85f);
  drawIcon(draw, icon, ImVec2(at.x + height * 0.5f + (label ? 2 : 0), at.y + height * 0.5f), height * 0.42f, colour);
  if (label) draw->AddText(ImVec2(at.x + height + 2, at.y + (height - ImGui::GetTextLineHeight()) * 0.5f), colour, label);
  if (hovered && tip) ImGui::SetTooltip("%s", tip);
  return pressed && enabled;
}

// A tab in the panel's bar: a word, a count, and a line under the chosen one.
bool panelTab(const char *label, int count, bool selected, bool flash) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  char countText[16];
  std::snprintf(countText, sizeof countText, "%d", count);
  const float labelWidth = ImGui::CalcTextSize(label).x;
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  const float countWidth = count >= 0 ? ImGui::CalcTextSize(countText).x + 10 : 0;
  ImGui::PopFont();
  const float height = ImGui::GetFrameHeight() + 4;
  const float width = labelWidth + (count >= 0 ? countWidth + 8 : 0) + 20;
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::PushID(label);
  const bool pressed = ImGui::InvisibleButton("##tab", ImVec2(width, height));
  const bool hovered = ImGui::IsItemHovered();
  ImGui::PopID();
  if (flash) draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), withAlpha(palette().red, 0.18f), 6.0f);
  else if (hovered && !selected) draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), text(0.05f), 6.0f);
  const float textY = at.y + (height - ImGui::GetTextLineHeight()) * 0.5f;
  draw->AddText(ImVec2(at.x + 10, textY), selected ? text() : secondary(), label);
  if (count >= 0) {
    const float cx = at.x + 10 + labelWidth + 6;
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
    const ImVec2 cs = ImGui::CalcTextSize(countText);
    const float cy = at.y + (height - cs.y - 4) * 0.5f;
    draw->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + cs.x + 10, cy + cs.y + 4),
                        count > 0 ? accent(selected ? 0.9f : 0.35f) : text(0.07f), (cs.y + 4) * 0.5f);
    draw->AddText(ImVec2(cx + 5, cy + 2), count > 0 && selected ? IM_COL32_WHITE : secondary(), countText);
    ImGui::PopFont();
  }
  if (selected) {
    draw->AddRectFilled(ImVec2(at.x + 8, at.y + height - 2.5f), ImVec2(at.x + width - 8, at.y + height),
                        accent(), 1.5f);
  }
  ImGui::SameLine(0, 2);
  return pressed;
}

// A small round button with an x, for taking a row away.
bool removeButton(const char *id) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float size = ImGui::GetTextLineHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 c(at.x + size * 0.5f, at.y + size * 0.5f);
  if (hovered) draw->AddCircleFilled(c, size * 0.5f, withAlpha(palette().red, 0.2f));
  const float r = size * 0.2f;
  const ImU32 colour = hovered ? palette().red : secondary();
  draw->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), colour, 1.5f);
  draw->AddLine(ImVec2(c.x + r, c.y - r), ImVec2(c.x - r, c.y + r), colour, 1.5f);
  return pressed;
}

const char *const KIND_BADGES[] = {"EXEC", "READ", "WRITE", "R/W", "SP", "SWITCH", "BEAM"};
const char *const BEAM_LABELS[] = {"VBL", "HBL", "Line", "Column", "Line + Col"};

ImU32 kindColour(Breakpoint::Kind kind, const Palette &p) {
  switch (kind) {
  case Breakpoint::Kind::Exec: return p.red;
  case Breakpoint::Kind::Read: return p.blue;
  case Breakpoint::Kind::Write: return p.orange;
  case Breakpoint::Kind::ReadWrite: return p.purple;
  case Breakpoint::Kind::Stack: return p.yellow;
  case Breakpoint::Kind::Switch: return p.green;
  case Breakpoint::Kind::Beam: return p.green;
  }
  return p.red;
}

} // namespace

CpuDebugger::CpuDebugger(Emulation &emulation, Platform &platform, Breakpoints &breakpoints)
    : emulation_(emulation), platform_(platform), breakpoints_(breakpoints) {}

void CpuDebugger::setMachine(const MachineProfile &profile) {
  profile_ = &profile;
  wide_ = profile.cpu == CPUVariant::CMOS_65C816;
  followPC_ = true;
  centreOn_.reset();
  selected_.reset();
  havePrevious_ = false;
  judgedResumes_ = NOT_JUDGED;
  hitId_ = 0;
  beamHitId_ = 0;
  reason_ = "Running";
  // A rebuilt machine's debugger holds none of them; the App applies the
  // list again before the next frame.
  breakpoints_.invalidate();
  emulation_.withMachine([&](host::MachineHost &host) { switches_ = host.softSwitches(); });
}

std::string CpuDebugger::formatAddress(uint32_t address) const {
  char out[16];
  if (wide_) std::snprintf(out, sizeof out, "%02X/%04X", (address >> 16) & 0xFF, address & 0xFFFF);
  else std::snprintf(out, sizeof out, "%04X", address & 0xFFFF);
  return out;
}

// ---------------------------------------------------------------------------
// Reading the machine
// ---------------------------------------------------------------------------

void CpuDebugger::take() {
  Snapshot s;
  const int rows = std::max(visibleRows_, 8);
  // A refill holding the machine keeps last frame's snapshot; what the
  // listing was asked to do waits for the next one.
  const bool took = emulation_.poll(poll_, [&](host::MachineHost &host) {
    if (!host.isBuilt()) return;
    s.valid = true;
    s.paused = host.isPaused();
    if (MachineDebug *debug = host.debug()) s.resumes = debug->resumeCount();
    s.cpu = host.cpuState();

    // Where the listing starts. Following the PC, it is left alone while the
    // PC is comfortably inside it, so stepping walks down the lines rather
    // than the lines scrolling under a fixed PC; it is recentred when the PC
    // leaves.
    if (jumpTo_) {
      viewTop_ = *jumpTo_;
      jumpTo_.reset();
    }
    if (centreOn_) {
      const auto around = host.disassembleRange(*centreOn_, CONTEXT_ABOVE, 1);
      viewTop_ = around.empty() ? *centreOn_ : around.front().address;
      centreOn_.reset();
      scrollPixels_ = 0;
    } else if (followPC_) {
      bool inside = false;
      const auto &lines = snapshot_.lines;
      const size_t top = snapshot_.topIndex;
      const size_t end = std::min(lines.size(), top + static_cast<size_t>(std::max(visibleRows_ - 3, 1)));
      for (size_t i = top; i < end; i++) {
        if (lines[i].address == s.cpu.pc) inside = i > top || lines[i].address == viewTop_;
      }
      if (!inside) {
        const auto around = host.disassembleRange(s.cpu.pc, CONTEXT_ABOVE, 1);
        viewTop_ = around.empty() ? s.cpu.pc : around.front().address;
        scrollPixels_ = 0;
      }
    }
    if (scrollBy_ > 0) {
      const auto down = host.disassembleRange(viewTop_, 0, scrollBy_ + 1);
      if (!down.empty()) viewTop_ = down.back().address;
    } else if (scrollBy_ < 0) {
      const auto up = host.disassembleRange(viewTop_, -scrollBy_, -scrollBy_ + 1);
      if (!up.empty()) viewTop_ = up.front().address;
    }
    scrollBy_ = 0;

    // A few lines either side of the window as well as the window, so a
    // scroll part of the way to the next line has something to show in the
    // gap it opens.
    auto listFrom = [&](uint32_t centre, int before) {
      s.lines = host.disassembleRange(centre, before, before + rows + LINES_BELOW);
      s.topIndex = 0;
      for (size_t i = 0; i < s.lines.size(); i++) {
        if (s.lines[i].address == centre) s.topIndex = i;
      }
    };
    listFrom(viewTop_, LINES_ABOVE);

    // Wherever the listing was scrolled to, the PC is shown as an
    // instruction when it is in view: decoded from an arbitrary start, the
    // instruction before it can swallow its first bytes, and the line the
    // machine is about to run never appears.
    const uint32_t pc = s.cpu.pc;
    if (s.topIndex < s.lines.size() && (pc & 0xFF0000) == (viewTop_ & 0xFF0000)) {
      const host::Instruction &last = s.lines.back();
      bool listed = false;
      int above = 0;
      for (size_t i = s.topIndex; i < s.lines.size(); i++) {
        if (s.lines[i].address == pc) listed = true;
        if (s.lines[i].address < pc) above++;
      }
      if (!listed && pc > viewTop_ && pc < last.address + last.length) {
        listFrom(pc, above + LINES_ABOVE);
        s.topIndex = s.topIndex >= static_cast<size_t>(above) ? s.topIndex - above : 0;
        viewTop_ = s.lines[s.topIndex].address;
      }
    }

    // What each line costs, and where the time has gone.
    s.cycleProfile = host.hasCycleProfile();
    s.coverage = host.hasCoverage();
    host.setProfiling(heatOn_);
    if (clearHeat_) {
      host.clearProfile();
      clearHeat_ = false;
      totalsAge_ = 0;
    }
    if (heatOn_ && s.cycleProfile && --totalsAge_ <= 0) {
      host.profileTotals(heatMax_, heatTotal_);
      totalsAge_ = 15; // a quarter of a second: 64K counters is not free
    }
    s.profiling = heatOn_;
    for (const host::Instruction &line : s.lines) {
      s.costs.push_back(host.cycleCost(line, s.cpu, s.paused && line.address == pc));
      s.heat.push_back(heatOn_ ? host.profileCycles(line.address) : 0);
      s.executed.push_back(heatOn_ && host.wasExecuted(line.address));
    }

    // The selection's total, walked from its first line to its last.
    if (selected_ && selectionEnd_ && *selected_ != *selectionEnd_) {
      const uint32_t lo = std::min(*selected_, *selectionEnd_);
      const uint32_t hi = std::max(*selected_, *selectionEnd_);
      for (const host::Instruction &line : host.disassembleRange(lo, 0, SELECTION_MAX)) {
        if (line.address > hi) break;
        const host::CycleCost c = host.cycleCost(line, s.cpu, s.paused && line.address == pc);
        s.selection.count++;
        s.selection.min += c.min;
        s.selection.max += c.max;
        s.selection.perByte = s.selection.perByte || c.perByte;
        if (heatOn_) s.selection.heat += host.profileCycles(line.address);
      }
    }

    s.current = host.disassemble(s.cpu.pc);
    s.effective = host.effectiveAddress(s.current, s.cpu);
    s.taken = host.branchTaken(s.current, s.cpu);

    // The stack, upward from the top: page one on a 6502 and in emulation
    // mode, anywhere in bank zero otherwise.
    const bool pageOne = !wide_ || s.cpu.emulation();
    auto stackAddress = [&](int i) -> uint32_t {
      return pageOne ? 0x0100 | ((s.cpu.sp + 1 + i) & 0xFF) : (s.cpu.sp + 1 + i) & 0xFFFF;
    };
    const int depth = pageOne ? std::min(STACK_ROWS, 0xFF - (s.cpu.sp & 0xFF)) : STACK_ROWS;
    for (int i = 0; i < std::max(depth, 0); i++) {
      StackEntry e;
      e.address = stackAddress(i);
      e.value = host.peek(e.address);
      s.stack.push_back(e);
    }
    // A return address is where a JSR or JSL left it: the byte before the
    // address it pushed (plus one) is that JSR's last operand byte.
    for (size_t i = 0; i + 1 < s.stack.size(); i++) {
      const uint32_t pushed = s.stack[i].value | (s.stack[i + 1].value << 8);
      const uint32_t bank = s.cpu.pc & 0xFF0000;
      if (i + 2 < s.stack.size() && wide_) {
        const uint32_t longPushed = pushed | (static_cast<uint32_t>(s.stack[i + 2].value) << 16);
        if (host.peek((longPushed - 3) & 0xFFFFFF) == 0x22) {
          s.stack[i].returnsTo = (longPushed + 1) & 0xFFFFFF;
          s.stack[i].longReturn = true;
          i += 2;
          continue;
        }
      }
      if (host.peek(bank | ((pushed - 2) & 0xFFFF)) == 0x20) {
        s.stack[i].returnsTo = bank | ((pushed + 1) & 0xFFFF);
        i += 1;
      }
    }

    for (const Watch &w : watches_) s.watchValues.push_back(host.evaluateExpression(w.expression));
    if (MachineDebug *debug = host.debug()) {
      s.traceCount = debug->traceCount();
      s.traceEnabled = debug->isTraceEnabled();
    }
  });
  if (took) snapshot_ = std::move(s);
}

void CpuDebugger::stopped(const std::string &reason) {
  reason_ = reason;
  stopCount_++;
  // This stop is judged: whatever the snapshot says next, it is not new until
  // the machine has been set running again.
  emulation_.withMachine([&](host::MachineHost &host) {
    if (MachineDebug *debug = host.debug()) judgedResumes_ = debug->resumeCount();
  });
  stoppedAt_ = ImGui::GetTime();
}

int CpuDebugger::importSymbols(const std::string &text) {
  const int count = symbols_.importSymbols(text);
  ImGui::MarkIniSettingsDirty();
  return count;
}

// Why the machine stopped, and whether it should have: a breakpoint whose
// condition is false sends it straight back to running.
void CpuDebugger::handleStop() {
  bool resume = false;
  bool running = false;
  bool otherStop = false;
  std::string reason;
  int hit = -1;
  int beamHit = -1;
  emulation_.withMachine([&](host::MachineHost &host) {
    MachineDebug *debug = host.debug();
    if (!debug || !host.isPaused()) {
      // Set running again since the snapshot: nothing to judge yet.
      running = true;
      return;
    }
    char line[160];
    // A condition that cannot be evaluated does not send the machine back
    // to running: it stops, and says why, which is the only way the user
    // learns the condition is wrong.
    std::string conditionProblem;
    auto conditionFails = [&](int index) {
      if (index < 0) return false;
      const std::string &condition = breakpoints_.all()[static_cast<size_t>(index)].condition;
      if (condition.empty()) return false;
      const bool holds = host.evaluateCondition(condition);
      conditionProblem = host.conditionError();
      if (!conditionProblem.empty()) return false;
      return !holds;
    };
    auto withProblem = [&](std::string text) {
      if (!conditionProblem.empty()) text += " (condition: " + conditionProblem + ")";
      return text;
    };
    if (debug->isTempBreakpointHit()) {
      std::snprintf(line, sizeof line, "Reached %s", formatAddress(debug->breakpointAddress()).c_str());
      reason = line;
      return;
    }
    otherStop = true;
    if (debug->isBreakpointHit()) {
      const uint32_t at = debug->breakpointAddress();
      hit = breakpoints_.execFor(at);
      if (conditionFails(hit)) {
        resume = true;
        return;
      }
      const Breakpoint *b = hit >= 0 ? &breakpoints_.all()[static_cast<size_t>(hit)] : nullptr;
      if (b && b->isRange()) {
        std::snprintf(line, sizeof line, "Entered %s-%s at %s", formatAddress(b->start).c_str(),
                      formatAddress(b->end).c_str(), formatAddress(at).c_str());
      } else {
        std::snprintf(line, sizeof line, "Breakpoint at %s", formatAddress(at).c_str());
      }
      reason = withProblem(line);
    } else if (debug->isWatchpointHit()) {
      const uint32_t at = debug->watchpointAddress();
      const bool write = debug->isWatchpointWrite();
      hit = breakpoints_.accessFor(at, write);
      if (conditionFails(hit)) {
        resume = true;
        return;
      }
      std::snprintf(line, sizeof line, write ? "Write %s \xE2\x86\x90 $%02X" : "Read %s \xE2\x86\x92 $%02X",
                    formatAddress(at).c_str(), debug->watchpointValue());
      reason = withProblem(line);
    } else if (debug->isStackBreakpointHit()) {
      hit = breakpoints_.stackFor(debug->stackBreakpointHitLow());
      if (conditionFails(hit)) {
        resume = true;
        return;
      }
      std::snprintf(line, sizeof line, "Stack pointer reached $%X", host.cpuState().sp);
      reason = withProblem(line);
    } else if (debug->isBeamBreakpointHit()) {
      beamHit = breakpoints_.beamFor(debug->beamBreakpointHitId());
      if (conditionFails(beamHit)) {
        resume = true;
        return;
      }
      std::snprintf(line, sizeof line, "Beam at line %d, position %d", debug->beamBreakScanline(),
                    debug->beamBreakHPos());
      reason = withProblem(line);
    } else if (debug->isSwitchBreakpointHit()) {
      // Said the way the Soft Switches window says it.
      hit = breakpoints_.switchFor(debug->switchBreakpointHitId());
      if (conditionFails(hit)) {
        resume = true;
        return;
      }
      reason = withProblem(host.switchHitText());
    } else {
      reason = "Paused";
    }
  });
  if (running) return;
  if (resume) {
    emulation_.withMachine([](host::MachineHost &host) { host.setPaused(false); });
    return;
  }
  // Any stop but the temporary breakpoint's own ends a step over, a step out
  // or a run to here: left armed, it would stop some later run at an address
  // the user has long forgotten.
  if (otherStop) {
    emulation_.withMachine([](host::MachineHost &host) {
      if (MachineDebug *debug = host.debug()) debug->clearTempBreakpoint();
    });
  }
  hitId_ = hit >= 0 ? breakpoints_.all()[static_cast<size_t>(hit)].id : 0;
  beamHitId_ = beamHit >= 0 ? breakpoints_.all()[static_cast<size_t>(beamHit)].id : 0;
  if (hit >= 0) breakpoints_.all()[static_cast<size_t>(hit)].hits++;
  if (beamHit >= 0) breakpoints_.all()[static_cast<size_t>(beamHit)].hits++;
  stopped(reason);
}

void CpuDebugger::update() {
  if (!profile_) return;
  take();
  if (!snapshot_.valid) return;

  if (!snapshot_.paused) {
    hitId_ = 0;
    beamHitId_ = 0;
    reason_ = "Running";
    return;
  }
  if (snapshot_.resumes != judgedResumes_) {
    const uint64_t before = judgedResumes_;
    handleStop();
    if (judgedResumes_ == before) return; // sent back to running
  }
  // A new stop, by cycle count: the registers are compared with the last
  // one, and the watches with their values then.
  if (snapshot_.cpu.cycles != lastStop_.cycles || !havePrevious_) {
    previousStop_ = havePrevious_ ? lastStop_ : snapshot_.cpu;
    lastStop_ = snapshot_.cpu;
    havePrevious_ = true;
    stoppedAtCycle_ = previousStop_.cycles;
    const bool first = previousStop_.cycles == lastStop_.cycles;
    for (size_t i = 0; i < watches_.size() && i < snapshot_.watchValues.size(); i++) {
      watches_[i].changed = !first && snapshot_.watchValues[i] != watches_[i].previous;
      watches_[i].previous = snapshot_.watchValues[i];
    }
  }
}

// The beam's place in the frame. Its line and column are counted in the
// machine's own picture (192 lines of 40 columns, a IIgs's Mega II included),
// and the profile's text rectangle says where that picture sits in the
// framebuffer: all of it on a //e, inside the border on a IIgs. A column is
// marked at its left edge, where the byte starts to be shifted out, as the
// browser marks it.
void CpuDebugger::beamOnScreen(bool windowOpen, float &x, float &y) const {
  x = y = -1.0f;
  if (!windowOpen || !profile_ || !snapshot_.valid || !snapshot_.paused) return;
  const MachineTiming &t = profile_->timing;
  const MachineDisplay &d = profile_->display;
  const BeamPosition &beam = snapshot_.cpu.beam;
  if (beam.scanline >= 0 && beam.scanline < t.visibleScanlines) {
    y = (d.textTop + (beam.scanline + 0.5f) * d.textHeight / t.visibleScanlines) / d.pixelHeight;
  }
  if (beam.column >= 0 && beam.column < t.visibleColumns && !beam.inHorizontalBlank) {
    x = (d.textLeft + static_cast<float>(beam.column) * d.textWidth / t.visibleColumns) / d.pixelWidth;
  }
}

// ---------------------------------------------------------------------------
// Running and stepping
// ---------------------------------------------------------------------------

void CpuDebugger::continueOrPause() {
  const bool pause = !snapshot_.paused;
  emulation_.withMachine([&](host::MachineHost &host) { host.setPaused(pause); });
  if (pause) {
    stopped("Paused");
    followPC_ = true;
  }
}

void CpuDebugger::stepInto() {
  emulation_.withMachine([](host::MachineHost &host) {
    host.setPaused(true);
    host.stepInstruction();
    host.forceRenderFrame();
  });
  stopped("Stepped");
  followPC_ = true;
}

void CpuDebugger::stepOver() {
  uint32_t to = 0;
  emulation_.withMachine([&](host::MachineHost &host) {
    host.setPaused(true);
    to = host.stepOver();
    if (!to) host.forceRenderFrame();
  });
  // With a call to step over, the machine runs to the instruction after it.
  if (!to) stopped("Stepped");
  followPC_ = true;
}

void CpuDebugger::stepOut() {
  uint32_t to = 0;
  emulation_.withMachine([&](host::MachineHost &host) {
    host.setPaused(true);
    to = host.stepOut();
    if (!to) host.forceRenderFrame();
  });
  if (!to) stopped("Stepped");
  followPC_ = true;
}

void CpuDebugger::runTo(uint32_t address) {
  emulation_.withMachine([&](host::MachineHost &host) {
    if (MachineDebug *debug = host.debug()) debug->setTempBreakpoint(address);
    host.setPaused(false);
  });
  followPC_ = true;
}

void CpuDebugger::setPC(uint32_t address) {
  emulation_.withMachine([&](host::MachineHost &host) {
    host.setRegister(host::CpuRegister::PC, address);
  });
}

void CpuDebugger::goTo(uint32_t address) {
  followPC_ = false;
  centreOn_ = address;
  selected_ = address;
}

bool CpuDebugger::addBreakpoint(Breakpoint::Kind kind, uint32_t start, uint32_t end) {
  Breakpoint b;
  b.kind = kind;
  b.start = start & addressMask();
  b.end = std::max(b.start, end & addressMask());
  if (!breakpoints_.add(b)) return false;
  tab_ = 0;
  ImGui::MarkIniSettingsDirty();
  return true;
}

void CpuDebugger::showInListing(uint32_t address) { goTo(address & addressMask()); }

int CpuDebugger::beamCount() const {
  int n = 0;
  for (const Breakpoint &b : breakpoints_.all()) n += b.kind == Breakpoint::Kind::Beam;
  return n;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void CpuDebugger::drawToolbar() {
  const bool paused = snapshot_.paused;
  ImGui::PushID("toolbar");
  if (toolButton("##run", paused ? Icon::Play : Icon::Pause, paused ? "Continue" : "Pause",
                 paused ? "Continue  (F5)" : "Pause  (F5)", true, true)) {
    continueOrPause();
  }
  ImGui::SameLine(0, 10);
  if (toolButton("##into", Icon::StepInto, "Step Into", "One instruction  (F11)")) stepInto();
  ImGui::SameLine(0, 2);
  if (toolButton("##over", Icon::StepOver, "Step Over", "Run a call through to its return  (F10)")) stepOver();
  ImGui::SameLine(0, 2);
  if (toolButton("##out", Icon::StepOut, "Step Out", "Run to the return from this routine  (Shift F11)")) stepOut();
  ImGui::SameLine(0, 14);

  // The state, and why it stopped: green while running, amber when paused,
  // red for a breakpoint, flashing briefly as it lands.
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const Palette p = palette();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const float height = ImGui::GetFrameHeight() + 6;
  const bool breakpoint = hitId_ || beamHitId_;
  const ImU32 colour = !paused ? p.green : breakpoint ? p.red : p.yellow;
  const float age = static_cast<float>(ImGui::GetTime() - stoppedAt_);
  const float flash = paused ? std::max(0.0f, 1.0f - age / 0.6f) : 0.0f;
  const std::string label = paused ? reason_ : "Running";
  const float textWidth = ImGui::CalcTextSize(label.c_str()).x;
  const ImVec2 end(at.x + textWidth + 34, at.y + height);
  draw->AddRectFilled(at, end, withAlpha(colour, 0.16f + 0.3f * flash), height * 0.5f);
  draw->AddRect(at, end, withAlpha(colour, 0.5f), height * 0.5f);
  const ImVec2 dot(at.x + 14, at.y + height * 0.5f);
  if (!paused) {
    // A slow breath while running, well inside the flash limits.
    const float breath = 0.6f + 0.4f * (0.5f + 0.5f * std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f));
    draw->AddCircleFilled(dot, 6.0f, withAlpha(colour, 0.25f * breath));
  }
  draw->AddCircleFilled(dot, 4.0f, colour);
  draw->AddText(ImVec2(at.x + 24, at.y + (height - ImGui::GetTextLineHeight()) * 0.5f), text(), label.c_str());
  ImGui::Dummy(ImVec2(end.x - at.x, height));

  // Since the last stop, while paused.
  if (paused && havePrevious_ && lastStop_.cycles > stoppedAtCycle_) {
    ImGui::SameLine(0, 12);
    const uint64_t delta = lastStop_.cycles - stoppedAtCycle_;
    const std::string since = "+" + grouped(delta) + (delta == 1 ? " cycle" : " cycles");
    const ImVec2 t = ImGui::GetCursorScreenPos();
    ImGui::PushFont(ui::monoFont(), 0.0f);
    draw->AddText(ImVec2(t.x, t.y + (height - ImGui::GetTextLineHeight()) * 0.5f), secondary(), since.c_str());
    const float w = ImGui::CalcTextSize(since.c_str()).x;
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(w, height));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Cycles since the stop before this one");
  }
  ImGui::PopID();
}

void CpuDebugger::drawRegisters() {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const Palette p = palette();
  const host::CpuState &c = snapshot_.cpu;
  const host::CpuState &before = previousStop_;
  const bool lit = snapshot_.paused && havePrevious_;
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float width = cardInner_;
  const int digits = wide_ ? 4 : 2;

  struct Tile {
    const char *name;
    host::CpuRegister reg;
    uint32_t value;
    uint32_t was;
    int digits;
  };
  std::vector<Tile> tiles = {
      {"A", host::CpuRegister::A, c.a, before.a, wide_ && !c.accumulator8() ? 4 : digits},
      {"X", host::CpuRegister::X, c.x, before.x, wide_ && !c.index8() ? 4 : digits},
      {"Y", host::CpuRegister::Y, c.y, before.y, wide_ && !c.index8() ? 4 : digits},
      {"SP", host::CpuRegister::SP, c.sp, before.sp, wide_ ? 4 : 2},
  };
  if (wide_) {
    tiles.push_back({"D", host::CpuRegister::D, c.d, before.d, 4});
    tiles.push_back({"DB", host::CpuRegister::DBR, c.dbr, before.dbr, 2});
  }

  caption(draw, start, "REGISTERS", secondary());
  float y = start.y + captionHeight() + 8;

  // The program counter gets a row of its own, with the routine it is in.
  {
    const ImVec2 a(start.x, y);
    const ImVec2 b(start.x + width, y + PC_HEIGHT);
    draw->AddRectFilled(a, b, well(), 8.0f);
    caption(draw, ImVec2(a.x + 10, a.y + 6), "PC", secondary());
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID("pc");
    if (editingRegister_ == 100) {
      ImGui::SetCursorScreenPos(ImVec2(a.x + 8, a.y + 18));
      ImGui::SetNextItemWidth(120);
      if (focusRegister_) ImGui::SetKeyboardFocusHere();
      if (ImGui::InputText("##edit", registerText_, sizeof registerText_,
                           ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsUppercase |
                               ImGuiInputTextFlags_AutoSelectAll)) {
        if (auto v = symbols_.resolve(registerText_, addressMask())) setPC(*v);
        editingRegister_ = -1;
      }
      // Escape, or a click elsewhere, leaves the PC as it was.
      if (ImGui::IsItemDeactivated()) editingRegister_ = -1;
      focusRegister_ = false;
    } else {
      ImGui::InvisibleButton("##tile", ImVec2(width, PC_HEIGHT));
      if (ImGui::IsItemHovered() && snapshot_.paused) {
        ImGui::SetTooltip("Double-click to set the PC");
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
          editingRegister_ = 100;
          focusRegister_ = true;
          std::snprintf(registerText_, sizeof registerText_, "%s", formatAddress(c.pc).c_str());
        }
      }
      ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 1.6f);
      const std::string pc = formatAddress(c.pc);
      draw->AddText(ImVec2(a.x + 10, a.y + 15), lit && c.pc != before.pc ? ui::accentText() : text(), pc.c_str());
      const float pcWidth = ImGui::CalcTextSize(pc.c_str()).x;
      ImGui::PopFont();
      // The nearest name at or before the PC, as "COUT+3".
      std::string where;
      if (auto sym = symbols_.lookup(c.pc)) where = sym->name;
      if (!where.empty()) {
        ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
        draw->AddText(ImVec2(a.x + 18 + pcWidth, a.y + 21), p.blue, where.c_str());
        ImGui::PopFont();
      }
      if (wide_) {
        char mode[24];
        std::snprintf(mode, sizeof mode, "%s", c.emulation() ? "EMULATION" : "NATIVE");
        ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
        const float mw = ImGui::CalcTextSize(mode).x;
        ImGui::PopFont();
        pill(draw, ImVec2(b.x - mw - 22, a.y + 6), mode, true, c.emulation() ? p.yellow : p.blue, ui::SMALL_TEXT);
      }
    }
    ImGui::PopID();
    y += PC_HEIGHT + 6;
  }

  // The rest in rows of equal tiles, so the card is the same height whatever
  // the values: four across for a 6502 (A X Y SP), three across in two rows
  // for a 65816 (A X Y, then SP D DB).
  const size_t perRow = wide_ ? 3 : 4;
  const float gap = 6;
  const float tileWidth = (width - gap * (perRow - 1)) / perRow;
  const float tileHeight = 46;
  for (size_t i = 0; i < tiles.size(); i++) {
    const Tile &t = tiles[i];
    const float x = start.x + (i % perRow) * (tileWidth + gap);
    const ImVec2 a(x, y);
    const ImVec2 b(x + tileWidth, y + tileHeight);
    const bool changed = lit && t.value != t.was;
    draw->AddRectFilled(a, b, changed ? accent(0.12f) : well(), 8.0f);
    if (changed) draw->AddRect(a, b, accent(0.45f), 8.0f);
    caption(draw, ImVec2(a.x + 10, a.y + 6), t.name, secondary());
    ImGui::PushID(t.name);
    ImGui::SetCursorScreenPos(a);
    const int index = static_cast<int>(i);
    if (editingRegister_ == index) {
      ImGui::SetCursorScreenPos(ImVec2(a.x + 6, a.y + 18));
      ImGui::SetNextItemWidth(tileWidth - 12);
      if (focusRegister_) ImGui::SetKeyboardFocusHere();
      if (ImGui::InputText("##edit", registerText_, sizeof registerText_,
                           ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsHexadecimal |
                               ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_AutoSelectAll)) {
        const uint32_t v = static_cast<uint32_t>(std::strtoul(registerText_, nullptr, 16));
        const host::CpuRegister reg = t.reg;
        emulation_.withMachine([&](host::MachineHost &host) { host.setRegister(reg, v); });
        editingRegister_ = -1;
      }
      if (ImGui::IsItemDeactivated()) editingRegister_ = -1;
      focusRegister_ = false;
    } else {
      ImGui::InvisibleButton("##tile", ImVec2(tileWidth, tileHeight));
      if (ImGui::IsItemHovered() && snapshot_.paused) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
          editingRegister_ = index;
          focusRegister_ = true;
          std::snprintf(registerText_, sizeof registerText_, "%0*X", t.digits, t.value);
        }
        const uint8_t low = static_cast<uint8_t>(t.value & 0x7F);
        if (low >= 0x20 && low < 0x7F && t.reg != host::CpuRegister::SP) {
          ImGui::SetTooltip("%u decimal  ·  '%c'  ·  double-click to edit", t.value, low);
        } else {
          ImGui::SetTooltip("%u decimal  ·  double-click to edit", t.value);
        }
      }
      char value[8];
      std::snprintf(value, sizeof value, "%0*X", t.digits, t.value);
      ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 1.3f);
      draw->AddText(ImVec2(a.x + 9, a.y + 17), changed ? ui::accentText() : text(), value);
      ImGui::PopFont();
      // The value in decimal, or where the stack pointer points; the
      // character it would print as is in the tooltip, for want of room.
      char extra[24];
      if (t.reg == host::CpuRegister::SP) {
        std::snprintf(extra, sizeof extra, "%04X", wide_ && !c.emulation() ? t.value : (0x100 | (t.value & 0xFF)));
      } else {
        std::snprintf(extra, sizeof extra, "%u", t.value);
      }
      ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
      const float ew = ImGui::CalcTextSize(extra).x;
      draw->AddText(ImVec2(b.x - ew - 8, a.y + 6), secondary(), extra);
      ImGui::PopFont();
    }
    ImGui::PopID();
    if (i % perRow == perRow - 1 || i + 1 == tiles.size()) y += tileHeight + gap;
  }
  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(width, y - start.y));
}

void CpuDebugger::drawFlags() {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const Palette p = palette();
  const host::CpuState &c = snapshot_.cpu;
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float width = cardInner_;

  char heading[32];
  std::snprintf(heading, sizeof heading, "FLAGS   P = $%02X", c.p);
  caption(draw, start, heading, secondary());
  float y = start.y + captionHeight() + 8;

  // A 65816 in native mode calls bits 5 and 4 M and X; a 6502 (and a 65816
  // emulating one) has an unused bit and Break there.
  const bool native = wide_ && !c.emulation();
  const char *names = native ? "NVMXDIZC" : "NV-BDIZC";
  static const char *const tips6502[] = {"Negative", "Overflow", "Unused", "Break",
                                         "Decimal", "Interrupt disable", "Zero", "Carry"};
  static const char *const tips816[] = {"Negative", "Overflow", "Accumulator 8-bit", "Index 8-bit",
                                        "Decimal", "Interrupt disable", "Zero", "Carry"};
  const ImU32 colours[8] = {p.red, p.orange, p.purple, p.purple, p.yellow, p.blue, p.green, p.green};
  const float gap = 4;
  const int count = 8 + (wide_ ? 1 : 0);
  const float size = std::min(30.0f, (width - gap * (count - 1)) / count);
  for (int bit = 0; bit < count; bit++) {
    const bool isE = bit == 8;
    const int mask = isE ? 0 : 0x80 >> bit;
    const bool set = isE ? c.emulation() : (c.p & mask) != 0;
    const bool changed = !isE && snapshot_.paused && havePrevious_ && ((c.p ^ previousStop_.p) & mask);
    const ImVec2 a(start.x + bit * (size + gap), y);
    const ImVec2 b(a.x + size, a.y + size);
    // The unused bit reads as one on a 6502 and means nothing, so it is
    // never lit as if it did.
    const bool unused = !isE && !native && bit == 2;
    const ImU32 colour = isE ? p.yellow : unused ? text(0.18f) : colours[bit];
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(bit);
    const bool clicked = ImGui::InvisibleButton("##flag", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    draw->AddRectFilled(a, b, set ? colour : text(hovered ? 0.1f : 0.05f), 7.0f);
    if (changed) draw->AddRect(ImVec2(a.x - 1.5f, a.y - 1.5f), ImVec2(b.x + 1.5f, b.y + 1.5f), accent(), 8.0f, 0, 1.5f);
    const char letter[2] = {isE ? 'E' : names[bit], 0};
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.95f);
    const ImVec2 ls = ImGui::CalcTextSize(letter);
    draw->AddText(ImVec2(a.x + (size - ls.x) * 0.5f, a.y + (size - ls.y) * 0.5f),
                  set && !unused ? ui::textOn(colour) : secondary(), letter);
    ImGui::PopFont();
    if (hovered) {
      if (isE) ImGui::SetTooltip("Emulation mode: %s", set ? "on" : "off");
      else ImGui::SetTooltip("%s: %d%s", (native ? tips816 : tips6502)[bit], set ? 1 : 0,
                             snapshot_.paused ? "  ·  click to flip" : "");
    }
    if (clicked && !isE && snapshot_.paused) {
      const uint8_t flipped = static_cast<uint8_t>(c.p ^ mask);
      emulation_.withMachine([&](host::MachineHost &host) { host.setRegister(host::CpuRegister::P, flipped); });
    }
  }
  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(width, y + size - start.y));
}

void CpuDebugger::drawClock() {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const Palette p = palette();
  const host::CpuState &c = snapshot_.cpu;
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float width = cardInner_;
  caption(draw, start, "CLOCK", secondary());
  float y = start.y + captionHeight() + 6;
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 1.15f);
  const std::string cycles = grouped(c.cycles);
  draw->AddText(ImVec2(start.x, y), text(), cycles.c_str());
  const float lineHeight = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  // The interrupt lines, lit while one is waiting.
  float x = start.x + width;
  const struct {
    const char *label;
    bool on;
    const char *tip;
  } lines[] = {{"EDGE", c.nmiEdge, "An NMI edge has been seen"},
               {"NMI", c.nmiPending, "Non-maskable interrupt pending"},
               {"IRQ", c.irqPending, "Interrupt request pending"}};
  for (const auto &line : lines) {
    if (wide_ && !std::strcmp(line.label, "EDGE")) continue;
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
    const float w = ImGui::CalcTextSize(line.label).x + 12;
    ImGui::PopFont();
    x -= w;
    pill(draw, ImVec2(x, y + 2), line.label, line.on, p.red, ui::SMALL_TEXT);
    ImGui::SetCursorScreenPos(ImVec2(x, y + 2));
    ImGui::InvisibleButton(line.label, ImVec2(w, 16));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", line.tip);
    x -= 4;
  }
  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(width, y + lineHeight - start.y));
}

// Where the beam is, as numbers. The picture of it is the screen itself:
// while the machine is paused the crosshair is drawn there (beamOnScreen), as
// the browser draws it, which shows what has been drawn this frame and what
// has not far better than a diagram of the raster could.
void CpuDebugger::drawBeam() {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const Palette p = palette();
  const host::CpuState &c = snapshot_.cpu;
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float width = cardInner_;
  caption(draw, start, "BEAM", secondary());
  char cycle[48];
  std::snprintf(cycle, sizeof cycle, "frame cycle %s", grouped(static_cast<uint64_t>(c.frameCycle)).c_str());
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  const float cw = ImGui::CalcTextSize(cycle).x;
  draw->AddText(ImVec2(start.x + width - cw, start.y), secondary(), cycle);
  ImGui::PopFont();

  const float y = start.y + captionHeight() + 6;
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 1.0f);
  const float lineHeight = ImGui::GetTextLineHeight();
  float x = start.x;
  auto field = [&](const char *name, int value, bool known) {
    char number[16];
    if (known) std::snprintf(number, sizeof number, "%d", value);
    else std::snprintf(number, sizeof number, "--");
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
    const float nw = ImGui::CalcTextSize(name).x;
    draw->AddText(ImVec2(x, y + lineHeight * 0.22f), secondary(), name);
    ImGui::PopFont();
    x += nw + 5;
    draw->AddText(ImVec2(x, y), known ? text() : secondary(), number);
    x += ImGui::CalcTextSize("000").x + 10;
  };
  field("LINE", c.beam.scanline, true);
  field("COL", c.beam.column, c.beam.column >= 0 && !c.beam.inVerticalBlank);
  field("H", c.beam.hPos, true);
  ImGui::PopFont();
  // Blanking, at the right.
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  const float hw = ImGui::CalcTextSize("HBL").x + 12;
  const float vw = ImGui::CalcTextSize("VBL").x + 12;
  ImGui::PopFont();
  const float py = y + (lineHeight - captionHeight() - 4) * 0.5f;
  pill(draw, ImVec2(start.x + width - hw, py), "HBL", c.beam.inHorizontalBlank, p.blue, ui::SMALL_TEXT);
  pill(draw, ImVec2(start.x + width - hw - 4 - vw, py), "VBL", c.beam.inVerticalBlank, p.blue, ui::SMALL_TEXT);
  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(width, y + lineHeight - start.y));
  if (ImGui::IsItemHovered() && snapshot_.paused) {
    ImGui::SetTooltip("The crosshair on the screen marks the beam while the machine is paused");
  }
}

// The stack takes whatever height the column has left, and scrolls inside
// it when there is more stack than room: the card ends at the window's
// bottom edge and only the list within it moves.
void CpuDebugger::drawStack(float height) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const Palette p = palette();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float width = cardInner_;
  caption(draw, start, "STACK", secondary());
  const float top = start.y + captionHeight() + 6;
  if (snapshot_.stack.empty()) draw->AddText(ImVec2(start.x, top), secondary(), "Empty");

  // The list overhangs the card's padding by the row highlight's margin.
  const float margin = 4;
  ImGui::SetCursorScreenPos(ImVec2(start.x - margin, top - 1));
  const float listHeight = std::max(0.0f, start.y + height - top + 1);
  if (!snapshot_.stack.empty() &&
      ImGui::BeginChild("##stack", ImVec2(width + margin * 2, listHeight), ImGuiChildFlags_None,
                        ImGuiWindowFlags_NoBackground)) {
    ImDrawList *list = ImGui::GetWindowDrawList();
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.9f);
    const float line = ImGui::GetTextLineHeight() + 3;
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    int skip = 0;
    for (size_t i = 0; i < snapshot_.stack.size(); i++) {
      const StackEntry &e = snapshot_.stack[i];
      const ImVec2 row = ImGui::GetCursorScreenPos();
      const float x = row.x + margin;
      const float y = row.y + 1;
      if (i == 0) list->AddRectFilled(row, ImVec2(row.x + rowWidth, row.y + line), accent(0.1f), 4.0f);
      char cell[48];
      std::snprintf(cell, sizeof cell, "%s", formatAddress(e.address).c_str());
      list->AddText(ImVec2(x, y), secondary(), cell);
      std::snprintf(cell, sizeof cell, "%02X", e.value);
      list->AddText(ImVec2(x + (wide_ ? 76 : 50), y), skip > 0 ? withAlpha(p.purple, 0.8f) : text(), cell);
      if (e.returnsTo) {
        // Where the RTS (or RTL) will go: the instruction after the call.
        std::string to = std::string(e.longReturn ? "RTL \xE2\x86\x92 " : "RTS \xE2\x86\x92 ") + formatAddress(*e.returnsTo);
        if (auto sym = symbols_.lookup(*e.returnsTo)) to += "  " + sym->name;
        list->AddText(ImVec2(x + (wide_ ? 104 : 78), y), p.purple, to.c_str());
        skip = e.longReturn ? 2 : 1;
      } else if (skip > 0) {
        skip--;
      }
      ImGui::Dummy(ImVec2(rowWidth, line));
      ImGui::SetCursorScreenPos(ImVec2(row.x, row.y + line));
    }
    ImGui::PopFont();
  }
  if (!snapshot_.stack.empty()) ImGui::EndChild();
  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(width, std::max(height, 0.0f)));
}

void CpuDebugger::drawSidebar(float width) {
  ImGui::BeginChild("##sidebar", ImVec2(width, 0), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
                        ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList *draw = ImGui::GetWindowDrawList();
  // A card around whatever the body draws; `fill` stretches the last one to
  // the bottom of the column.
  auto section = [&](auto &&body, bool fill) {
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float bottomEdge = a.y + ImGui::GetContentRegionAvail().y;
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(ImVec2(a.x + PAD, a.y + PAD));
    cardInner_ = w - PAD * 2;
    cardHeight_ = bottomEdge - a.y - PAD * 2;
    ImGui::PushItemWidth(w - PAD * 2);
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2(w - PAD * 2, 0));
    body();
    ImGui::EndGroup();
    ImGui::PopItemWidth();
    const float bottom = fill ? bottomEdge : ImGui::GetItemRectMax().y + PAD;
    draw->ChannelsSetCurrent(0);
    card(draw, a, ImVec2(a.x + w, bottom));
    draw->ChannelsMerge();
    // The card filling the column ends at its foot, so the cursor goes
    // no further than that: past it, the child would have content to scroll.
    ImGui::SetCursorScreenPos(ImVec2(a.x, fill ? bottom : bottom + 8));
    if (fill) ImGui::Dummy(ImVec2(0, 0));
  };
  section([&] { drawRegisters(); }, false);
  section([&] { drawFlags(); }, false);
  section([&] {
    drawClock();
    ImGui::Dummy(ImVec2(0, 4));
    drawBeam();
  }, false);
  section([&] { drawStack(cardHeight_); }, true);
  ImGui::EndChild();
}

std::string CpuDebugger::symbolised(const host::Instruction &in) const {
  // An operand's address, written as its name where it has one: "$FDED"
  // becomes "COUT", "$24" becomes "CH". An immediate is left alone.
  if (in.operand.empty() || in.mode == host::OperandMode::Immediate) return in.operand;
  std::string out;
  const std::string &op = in.operand;
  for (size_t i = 0; i < op.size();) {
    if (op[i] == '$') {
      size_t j = i + 1;
      while (j < op.size() && std::isxdigit(static_cast<unsigned char>(op[j]))) j++;
      const std::string hex = op.substr(i + 1, j - i - 1);
      // A bank and a slash belong to the address after them.
      if (j < op.size() && op[j] == '/' && hex.size() == 2) {
        size_t k = j + 1;
        while (k < op.size() && std::isxdigit(static_cast<unsigned char>(op[k]))) k++;
        out += op.substr(i, k - i);
        i = k;
        continue;
      }
      if (hex.size() == 2 || hex.size() == 4) {
        const uint32_t address = static_cast<uint32_t>(std::strtoul(hex.c_str(), nullptr, 16));
        // A branch's target is in the program's bank.
        const uint32_t full = in.mode == host::OperandMode::Relative || in.mode == host::OperandMode::RelativeLong
                                  ? in.target
                                  : address;
        if (auto sym = symbols_.lookup(full); sym && (hex.size() == 4 || full < 0x100)) {
          out += sym->name;
          i = j;
          continue;
        }
      }
      out += op.substr(i, j - i);
      i = j;
    } else {
      out += op[i++];
    }
  }
  return out;
}

void CpuDebugger::drawCodeHeader() {
  ImGui::PushID("codeheader");
  // One row, every control a frame's height: navigation, then the heat
  // switch, then bookmarks and symbols, with a wider gap between groups.
  constexpr float GROUP_GAP = 14.0f;
  constexpr float ITEM_GAP = 6.0f;
  // A remove button is a line high; centre it on the row.
  auto centredRemove = [](const char *id) {
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f);
    return removeButton(id);
  };
  if (toolButton("##back", Icon::Back, nullptr, "Back  (\xE2\x8C\x98[)", canGoBack(), false, true)) back();
  ImGui::SameLine(0, 0);
  if (toolButton("##forward", Icon::Forward, nullptr, "Forward  (\xE2\x8C\x98])", canGoForward(), false, true)) {
    forward();
  }
  ImGui::SameLine(0, ITEM_GAP);
  ImGui::SetNextItemWidth(150);
  if (gotoBad_) ImGui::PushStyleColor(ImGuiCol_FrameBg, withAlpha(palette().red, 0.18f));
  const bool go = ImGui::InputTextWithHint("##goto", "Go to address or name", gotoText_, sizeof gotoText_,
                                           ImGuiInputTextFlags_EnterReturnsTrue);
  if (gotoBad_) ImGui::PopStyleColor();
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("$FDED, FDED, COUT%s", wide_ ? ", E1/2000" : "");
  if (go) {
    if (auto at = symbols_.resolve(gotoText_, addressMask())) {
      navigate(*at);
      gotoBad_ = false;
    } else {
      gotoBad_ = true;
    }
  }
  if (ImGui::IsItemEdited()) gotoBad_ = false;
  ImGui::SameLine(0, ITEM_GAP);
  if (toolButton("##follow", Icon::Follow, nullptr, followPC_ ? "Following the PC  (Home)" : "Follow the PC  (Home)",
                 true, followPC_, true)) {
    followPC_ = true;
    centreOn_ = snapshot_.cpu.pc;
  }

  // Heat on a //e, which counts the cycles spent at every address; coverage
  // alone on a IIgs, which records only what has run.
  ImGui::SameLine(0, GROUP_GAP);
  const char *heatLabel = snapshot_.cycleProfile ? "Heat" : "Coverage";
  if (ui::Switch(heatLabel, &heatOn_)) {
    clearHeat_ = heatOn_;
    ImGui::MarkIniSettingsDirty();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(snapshot_.cycleProfile
                          ? "Shade each line by the cycles spent there, and dim what has never run"
                          : "Dim what has never run (a IIgs records what has run, not the time it took)");
  }
  if (heatOn_) {
    ImGui::SameLine(0, ITEM_GAP);
    if (centredRemove("##clearheat")) clearHeat_ = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Start counting again");
  }

  ImGui::SameLine(0, GROUP_GAP);
  drawBookmarks();
  if (!bookmarks_.empty()) ImGui::SameLine(0, ITEM_GAP);
  if (ui::Button("Symbols\xE2\x80\xA6")) {
    if (platform_.openFile) {
      platform_.openFile("Import Symbols", {"dbg", "sym", "txt", "labels", "map", "s", "lst"},
                         [this](const std::string &path) {
                           if (path.empty()) return;
                           if (auto bytes = readFile(path)) {
                             const int count = symbols_.importSymbols(std::string(bytes->begin(), bytes->end()));
                             char message[96];
                             std::snprintf(message, sizeof message, "%d symbols imported", count);
                             importMessage_ = message;
                             ImGui::MarkIniSettingsDirty();
                           }
                         });
    }
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Import symbols: ca65 .dbg, Merlin EQU, ACME = or \"$ADDR NAME\" lists%s%s",
                      importMessage_.empty() ? "" : "\n", importMessage_.c_str());
  }
  if (symbols_.importedCount()) {
    ImGui::SameLine(0, ITEM_GAP);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%zu", symbols_.importedCount());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%zu symbols imported", symbols_.importedCount());
    ImGui::SameLine(0, 4);
    if (centredRemove("##clearimported")) {
      symbols_.clearImported();
      ImGui::MarkIniSettingsDirty();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Forget the imported symbols");
  }
  ImGui::PopID();
}

bool CpuDebugger::isBookmarked(uint32_t address) const {
  return std::binary_search(bookmarks_.begin(), bookmarks_.end(), address);
}

void CpuDebugger::toggleBookmark(uint32_t address) {
  const auto it = std::lower_bound(bookmarks_.begin(), bookmarks_.end(), address);
  if (it != bookmarks_.end() && *it == address) bookmarks_.erase(it);
  else bookmarks_.insert(it, address);
  ImGui::MarkIniSettingsDirty();
}

// The bookmarks, as a list to jump to. Offered only once there is one.
void CpuDebugger::drawBookmarks() {
  if (bookmarks_.empty()) return;
  char label[32];
  std::snprintf(label, sizeof label, "Bookmarks %zu", bookmarks_.size());
  if (ui::Button(label)) ImGui::OpenPopup("##bookmarks");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("\xE2\x8C\x98-click in the gutter to mark a line");
  if (!ImGui::BeginPopup("##bookmarks")) return;
  std::optional<uint32_t> remove;
  for (uint32_t address : bookmarks_) {
    ImGui::PushID(static_cast<int>(address));
    std::string label = formatAddress(address);
    if (auto sym = symbols_.lookup(address)) label += "  " + sym->name;
    ImGui::PushFont(ui::monoFont(), 0.0f);
    if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_None, ImVec2(220, 0))) goTo(address);
    ImGui::PopFont();
    ImGui::SameLine(0, 8);
    if (removeButton("##remove")) remove = address;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove the bookmark");
    ImGui::PopID();
  }
  ImGui::Separator();
  if (ImGui::MenuItem("Clear All")) {
    bookmarks_.clear();
    ImGui::MarkIniSettingsDirty();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
  if (remove) toggleBookmark(*remove);
}

// Where an instruction goes, when the listing can say: a branch, or a jump
// or call to an address written in it. An indirect jump's target is wherever
// a pointer says, which is not this line's to know.
static bool hasKnownTarget(const host::Instruction &in) {
  if (in.flow != FlowType::CALL && in.flow != FlowType::UNCONDITIONAL && in.flow != FlowType::CONDITIONAL) {
    return false;
  }
  switch (in.mode) {
  case host::OperandMode::Relative:
  case host::OperandMode::RelativeLong:
  case host::OperandMode::DirectRelative:
  case host::OperandMode::Absolute:
  case host::OperandMode::AbsoluteLong: return true;
  default: return false;
  }
}

static std::string formatCost(const host::CycleCost &c) {
  char out[16];
  if (c.perByte) std::snprintf(out, sizeof out, "%u/b", c.min);
  else if (c.min == c.max) std::snprintf(out, sizeof out, "%u", c.min);
  else std::snprintf(out, sizeof out, "%u-%u", c.min, c.max);
  return out;
}

void CpuDebugger::navigate(uint32_t target) {
  backStack_.push_back(viewTop_);
  forwardStack_.clear();
  goTo(target);
}

void CpuDebugger::back() {
  if (backStack_.empty()) return;
  forwardStack_.push_back(viewTop_);
  jumpTo_ = backStack_.back();
  backStack_.pop_back();
  followPC_ = false;
  scrollPixels_ = 0;
}

void CpuDebugger::forward() {
  if (forwardStack_.empty()) return;
  backStack_.push_back(viewTop_);
  jumpTo_ = forwardStack_.back();
  forwardStack_.pop_back();
  followPC_ = false;
  scrollPixels_ = 0;
}

void CpuDebugger::drawCode(ImVec2 size) {
  ImGui::BeginChild("##code", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const Palette p = palette();
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  const ImVec2 end(origin.x + avail.x, origin.y + avail.y);
  draw->AddRectFilled(origin, end, well(), 8.0f);
  draw->AddRect(origin, end, ImGui::GetColorU32(ImGuiCol_Border), 8.0f);

  ImGui::PushFont(ui::monoFont(), 0.0f);
  const float lineHeight = ImGui::GetTextLineHeight() + 5;
  const float charWidth = ImGui::CalcTextSize("0").x;
  visibleRows_ = std::max(8, static_cast<int>((avail.y - 8) / lineHeight) + 1);
  const ImGuiIO &io = ImGui::GetIO();

  // The columns: the gutter, the arrows, the address, the bytes, the cost,
  // then the instruction. The scrollbar takes the right-hand edge.
  const float gutter = origin.x + 6;
  const float lanesLeft = gutter + 24;
  const float lanesRight = lanesLeft + ARROW_LANES * ARROW_PITCH;
  const float headBase = lanesRight + ARROW_APPROACH;
  const float addressX = headBase + ARROW_HEAD_LENGTH + 3;
  const float bytesX = addressX + charWidth * (wide_ ? 8.5f : 5.5f);
  const float cyclesX = bytesX + charWidth * (wide_ ? 12.5f : 9.5f);
  const float mnemonicX = cyclesX + charWidth * 5.5f;
  const float operandX = mnemonicX + charWidth * 5;
  const float listRight = end.x - SCROLLBAR_WIDTH - 6;

  // Scrolling, in pixels: three lines to a notch of a wheel, and a trackpad
  // moving the code about as far as the fingers go.
  const bool hovered = ImGui::IsWindowHovered();
  if (hovered && io.MouseWheel != 0 && !draggingBar_) {
    scrollPixels_ -= io.MouseWheel * lineHeight * 3;
    followPC_ = false;
  }
  if (hovered && ImGui::IsMouseClicked(3)) back();
  if (hovered && ImGui::IsMouseClicked(4)) forward();
  if (ImGui::IsWindowFocused() && !ImGui::GetIO().WantTextInput) {
    const int page = std::max(1, visibleRows_ - 3);
    auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };
    if (key(ImGuiKey_DownArrow)) scrollBy_ += 1, followPC_ = false;
    if (key(ImGuiKey_UpArrow)) scrollBy_ -= 1, followPC_ = false;
    if (key(ImGuiKey_PageDown)) scrollBy_ += page, followPC_ = false;
    if (key(ImGuiKey_PageUp)) scrollBy_ -= page, followPC_ = false;
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
      followPC_ = true;
      centreOn_ = snapshot_.cpu.pc;
    }
  }

  // Where every line goes: a name heads its own line, as an assembler
  // listing puts a label, and the top line is drawn scrollPixels_ above the
  // listing's top edge.
  const auto &lines = snapshot_.lines;
  const size_t count = lines.size();
  const size_t top = std::min(snapshot_.topIndex, count ? count - 1 : 0);
  std::vector<float> blockTop(count), blockHeight(count);
  std::vector<std::optional<DebugSymbols::Symbol>> heads(count);
  for (size_t i = 0; i < count; i++) {
    if (auto sym = symbols_.lookup(lines[i].address);
        sym && sym->category != DebugSymbols::Category::ZeroPage && sym->category != DebugSymbols::Category::SoftSwitch) {
      heads[i] = sym;
    }
    blockHeight[i] = lineHeight * (heads[i] ? 2 : 1);
  }
  if (count) {
    blockTop[top] = origin.y + 4 - scrollPixels_;
    for (size_t i = top + 1; i < count; i++) blockTop[i] = blockTop[i - 1] + blockHeight[i - 1];
    for (size_t i = top; i-- > 0;) blockTop[i] = blockTop[i + 1] - blockHeight[i];
  }
  auto rowY = [&](size_t i) { return blockTop[i] + (heads[i] ? lineHeight : 0); };
  auto visible = [&](size_t i) { return rowY(i) + lineHeight > origin.y && rowY(i) < end.y; };

  const uint32_t pc = snapshot_.cpu.pc;
  const bool showHeat = snapshot_.profiling && snapshot_.cycleProfile && heatMax_ > 0;
  const bool showCoverage = snapshot_.profiling && snapshot_.coverage;
  std::optional<uint32_t> selLo, selHi;
  if (selected_ && selectionEnd_) {
    selLo = std::min(*selected_, *selectionEnd_);
    selHi = std::max(*selected_, *selectionEnd_);
  }
  int hoveredRow = -1;

  draw->PushClipRect(origin, end, true);
  for (size_t i = 0; i < count; i++) {
    if (blockTop[i] + blockHeight[i] < origin.y || blockTop[i] > end.y) continue;
    const host::Instruction &in = lines[i];
    if (heads[i]) {
      const std::string label = heads[i]->name + ":";
      draw->AddText(ImVec2(addressX, blockTop[i] + 2), symbolColour(heads[i]->category, p), label.c_str());
    }
    const float y = rowY(i);
    const bool isPC = in.address == pc;
    const bool hasBreakpoint = breakpoints_.hasExecAt(in.address);
    const bool inSelection = selLo ? in.address >= *selLo && in.address <= *selHi : selected_ && *selected_ == in.address;
    const ImVec2 rowA(origin.x + 2, y);
    const ImVec2 rowB(listRight, y + lineHeight);
    // Never run since heat went on: probably data, or code nothing reached.
    const bool cold = showCoverage && i < snapshot_.executed.size() && !snapshot_.executed[i];
    const float ink = cold ? 0.4f : 1.0f;

    // The operand, with names for addresses that have them.
    const std::string operand = symbolised(in);
    const bool named = operand != in.operand;
    const float afterOperand = operandX + ImGui::CalcTextSize(operand.c_str()).x;
    const bool followable = hasKnownTarget(in);

    ImGui::SetCursorScreenPos(rowA);
    ImGui::PushID(static_cast<int>(in.address));
    ImGui::InvisibleButton("##row", ImVec2(rowB.x - rowA.x, lineHeight));
    const bool rowHovered = ImGui::IsItemHovered();
    if (rowHovered) hoveredRow = static_cast<int>(i);
    const float mx = io.MousePos.x;
    const bool overOperand = rowHovered && followable && mx >= operandX && mx <= afterOperand;
    const bool overCycles = rowHovered && mx >= cyclesX - 4 && mx < mnemonicX - 4;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
      if (mx < lanesLeft && io.KeyCtrl) {
        // Command-click, as the browser's Ctrl/Cmd-click: ImGui's Ctrl is ⌘ on a Mac.
        toggleBookmark(in.address);
      } else if (mx < lanesLeft) {
        breakpoints_.toggleExec(in.address);
        ImGui::MarkIniSettingsDirty();
      } else if (overOperand) {
        navigate(in.target);
      } else if (io.KeyShift && selected_) {
        selectionEnd_ = in.address;
      } else {
        selected_ = in.address;
        selectionEnd_.reset();
      }
    }
    if (rowHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && mx >= lanesLeft && !overOperand) {
      runTo(in.address);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
      menuAddress_ = in.address;
      if (!inSelection) {
        selected_ = in.address;
        selectionEnd_.reset();
      }
      ImGui::OpenPopup("##linemenu");
    }
    ImGui::PopID();

    // Where the time went: warmer for more of it, on a logarithmic scale so
    // a loop that has run a million times does not leave the rest blank.
    if (showHeat && i < snapshot_.heat.size() && snapshot_.heat[i]) {
      const float t = static_cast<float>(std::log1p(static_cast<double>(snapshot_.heat[i])) /
                                         std::log1p(static_cast<double>(heatMax_)));
      const ImU32 warm = t > 0.66f ? p.red : t > 0.33f ? p.orange : p.yellow;
      draw->AddRectFilled(rowA, rowB, withAlpha(warm, 0.04f + 0.14f * t), 4.0f);
      draw->AddRectFilled(ImVec2(rowB.x - 4, rowA.y + 2), ImVec2(rowB.x - 1, rowB.y - 2), withAlpha(warm, 0.4f + 0.6f * t), 1.0f);
    }
    if (isPC) {
      draw->AddRectFilled(rowA, rowB, accent(0.2f), 4.0f);
      draw->AddRectFilled(rowA, ImVec2(rowA.x + 3, rowB.y), accent(), 1.5f);
    } else if (inSelection) {
      draw->AddRectFilled(rowA, rowB, accent(0.1f), 4.0f);
    } else if (hasBreakpoint) {
      draw->AddRectFilled(rowA, rowB, withAlpha(p.red, 0.1f), 4.0f);
    } else if (rowHovered) {
      draw->AddRectFilled(rowA, rowB, text(0.04f), 4.0f);
    }

    // The gutter: a breakpoint's dot, the PC's arrow, or a hint of either.
    const ImVec2 dot(gutter + 9, y + lineHeight * 0.5f);
    if (hasBreakpoint) {
      bool enabled = false;
      for (const Breakpoint &b : breakpoints_.all()) {
        if (b.kind == Breakpoint::Kind::Exec && !b.isRange() && b.start == in.address) enabled = b.enabled;
      }
      if (enabled) draw->AddCircleFilled(dot, 5.5f, p.red);
      else draw->AddCircle(dot, 5.0f, p.red, 0, 1.5f);
    } else if (rowHovered && mx < lanesLeft) {
      draw->AddCircleFilled(dot, 5.0f, withAlpha(io.KeyCtrl ? p.blue : p.red, 0.35f));
    }
    if (isBookmarked(in.address)) {
      const float rx = rowB.x - 18;
      const float ry = y + 3;
      const float rh = lineHeight - 6;
      draw->AddRectFilled(ImVec2(rx, ry), ImVec2(rx + 9, ry + rh), p.blue, 1.5f);
      draw->AddTriangleFilled(ImVec2(rx, ry + rh + 0.5f), ImVec2(rx + 9, ry + rh + 0.5f),
                              ImVec2(rx + 4.5f, ry + rh - 4), isPC ? accent(0.2f) : well());
    }
    if (isPC) {
      const float ax = gutter + 15;
      draw->AddTriangleFilled(ImVec2(ax, dot.y - 5), ImVec2(ax, dot.y + 5), ImVec2(ax + 7, dot.y), accent());
    }

    const float textY = y + 2.5f;
    const std::string address = formatAddress(in.address);
    draw->AddText(ImVec2(addressX, textY), isPC ? text() : text(0.55f * ink), address.c_str());
    char bytes[16] = "";
    for (int b = 0; b < in.length && b < 4; b++) {
      char one[4];
      std::snprintf(one, sizeof one, b ? " %02X" : "%02X", in.bytes[b]);
      std::strcat(bytes, one);
    }
    draw->AddText(ImVec2(bytesX, textY), text(0.4f * ink), bytes);

    // The cost: exact at the PC, where the registers decide it.
    if (i < snapshot_.costs.size()) {
      const host::CycleCost &c = snapshot_.costs[i];
      const std::string cost = formatCost(c);
      const bool exact = isPC && snapshot_.paused;
      draw->AddText(ImVec2(cyclesX, textY), exact ? ui::accentText() : text(0.45f * ink), cost.c_str());
      if (overCycles) {
        std::string tip;
        if (c.perByte) tip = "7 cycles for every byte moved";
        else if (c.min == c.max) tip = std::to_string(c.min) + (c.min == 1 ? " cycle" : " cycles");
        else tip = std::to_string(c.min) + " to " + std::to_string(c.max) + " cycles: a page crossed or a branch taken costs more";
        if (exact) tip += ", as the registers stand";
        if (wide_) tip += "\nProcessor cycles: a Mega II access stretches one to the 1MHz clock";
        if (showHeat && i < snapshot_.heat.size()) {
          char share[96];
          std::snprintf(share, sizeof share, "\n%s cycles spent here (%.2f%%)", grouped(snapshot_.heat[i]).c_str(),
                        heatTotal_ ? 100.0 * snapshot_.heat[i] / static_cast<double>(heatTotal_) : 0.0);
          tip += share;
        }
        if (cold) tip += "\nNot run since heat was switched on";
        ImGui::SetTooltip("%s", tip.c_str());
      }
    }

    draw->AddText(ImVec2(mnemonicX, textY), withAlpha(categoryColour(in.category, p), ink), in.mnemonic.c_str());
    const ImU32 operandColour = in.mode == host::OperandMode::Immediate ? p.yellow : named ? p.blue : text();
    draw->AddText(ImVec2(operandX, textY), withAlpha(operandColour, ink), operand.c_str());
    if (overOperand) {
      // A link: underlined, and a hand, because a click follows it.
      draw->AddLine(ImVec2(operandX, textY + ImGui::GetTextLineHeight()), ImVec2(afterOperand, textY + ImGui::GetTextLineHeight()),
                    withAlpha(operandColour, 0.8f));
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      ImGui::SetTooltip("Go to %s%s", formatAddress(in.target).c_str(), named ? (" (" + in.operand + ")").c_str() : "");
    } else if (named && rowHovered && mx > operandX && mx < afterOperand) {
      ImGui::SetTooltip("%s", in.operand.c_str());
    }

    // What the instruction at the PC is about to do.
    float noteX = std::max(afterOperand + 18, operandX + charWidth * 16);
    if (isPC && snapshot_.paused) {
      char note[64] = "";
      ImU32 noteColour = text(0.7f);
      if (snapshot_.taken) {
        std::snprintf(note, sizeof note, "%s", *snapshot_.taken ? "taken" : "not taken");
        noteColour = *snapshot_.taken ? p.green : p.orange;
      } else if (snapshot_.effective) {
        const bool jump = in.flow != FlowType::SEQUENTIAL;
        if (jump) std::snprintf(note, sizeof note, "\xE2\x86\x92 %s", formatAddress(snapshot_.effective->address).c_str());
        else std::snprintf(note, sizeof note, "%s = %02X", formatAddress(snapshot_.effective->address).c_str(),
                           snapshot_.effective->value);
        noteColour = p.blue;
      }
      if (note[0]) {
        ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
        const ImVec2 ns = ImGui::CalcTextSize(note);
        const ImVec2 na(noteX, y + (lineHeight - ns.y - 4) * 0.5f);
        draw->AddRectFilled(na, ImVec2(na.x + ns.x + 12, na.y + ns.y + 4), withAlpha(noteColour, 0.16f), (ns.y + 4) * 0.5f);
        draw->AddText(ImVec2(na.x + 6, na.y + 2), noteColour, note);
        noteX = na.x + ns.x + 24;
        ImGui::PopFont();
      }
    }
    if (const DebugSymbols::Label *l = symbols_.label(in.address); l && !l->comment.empty()) {
      const std::string comment = "; " + l->comment;
      draw->AddText(ImVec2(noteX, textY), text(0.45f), comment.c_str());
    }
  }

  // Branch and jump arrows, in lanes between the gutter and the addresses:
  // the shortest nearest the code, so nested loops read as nested. One whose
  // other end is off the listing runs to the edge it is beyond.
  struct Arrow {
    float from, to;
    bool toVisible;
    bool conditional;
    bool fromPC;
    bool hot;
    int lane = -1;
  };
  std::vector<Arrow> arrows;
  if (count) {
    size_t first = count, last = 0;
    for (size_t i = 0; i < count; i++) {
      if (visible(i)) {
        first = std::min(first, i);
        last = std::max(last, i);
      }
    }
    for (size_t i = first; i <= last && i < count; i++) {
      const host::Instruction &in = lines[i];
      if (in.flow == FlowType::CALL || !hasKnownTarget(in)) continue;
      if ((in.target & 0xFF0000) != (in.address & 0xFF0000)) continue;
      Arrow a{};
      a.from = rowY(i) + lineHeight * 0.5f;
      a.conditional = in.flow == FlowType::CONDITIONAL;
      a.fromPC = in.address == pc && snapshot_.paused;
      a.hot = static_cast<int>(i) == hoveredRow;
      a.toVisible = false;
      for (size_t j = first; j <= last; j++) {
        if (lines[j].address == in.target) {
          a.to = rowY(j) + lineHeight * 0.5f;
          a.toVisible = true;
          if (static_cast<int>(j) == hoveredRow) a.hot = true;
        }
      }
      if (!a.toVisible) a.to = in.target < in.address ? origin.y + 2 : end.y - 2;
      arrows.push_back(a);
    }
  }
  std::sort(arrows.begin(), arrows.end(),
            [](const Arrow &a, const Arrow &b) { return std::fabs(a.to - a.from) < std::fabs(b.to - b.from); });
  std::vector<std::vector<std::pair<float, float>>> lanes(ARROW_LANES);
  for (Arrow &a : arrows) {
    const float lo = std::min(a.from, a.to) - 3;
    const float hi = std::max(a.from, a.to) + 3;
    for (int l = 0; l < ARROW_LANES && a.lane < 0; l++) {
      bool free = true;
      for (const auto &[s0, s1] : lanes[l]) free = free && (hi < s0 || lo > s1);
      if (free) {
        a.lane = l;
        lanes[l].push_back({lo, hi});
      }
    }
  }
  // Each arrow is one stroke with rounded corners, so the turns join rather
  // than overlap. It leaves its lane on a slant and meets its head along
  // that slant, stopping at the head's base instead of running into the
  // point. The slope is the same from every lane, so arrows arriving at one
  // line from one side merge into a single approach and share a head, drawn
  // once after every stroke in the colour of the strongest arrow there.
  // Coordinates sit on pixel centres so a thin line stays crisp.
  auto crisp = [](float v) { return std::floor(v) + 0.5f; };
  const float base = crisp(headBase);
  struct Head {
    ImU32 colour;
    int rank;
    ImVec2 at, along;
  };
  std::map<std::pair<int, int>, Head> arrowHeads;
  // Weakest first, so the hovered arrow and the PC's are drawn over the rest.
  std::stable_sort(arrows.begin(), arrows.end(), [](const Arrow &a, const Arrow &b) {
    return (a.hot || a.fromPC) < (b.hot || b.fromPC);
  });
  for (const Arrow &a : arrows) {
    if (a.lane < 0) continue;
    const float x = crisp(lanesRight - (a.lane + 0.5f) * ARROW_PITCH);
    const float from = crisp(a.from);
    const float to = crisp(a.to);
    ImU32 colour = a.conditional ? p.purple : p.blue;
    if (a.fromPC && snapshot_.taken) colour = *snapshot_.taken ? p.green : withAlpha(p.orange, 0.6f);
    const bool strong = a.hot || a.fromPC;
    colour = withAlpha(colour, strong ? 1.0f : 0.55f);
    const float thick = strong ? 2.0f : 1.0f;
    const float dir = to < from ? -1.0f : 1.0f;
    const float span = std::fabs(to - from);
    const float radius = std::min(4.0f, span * 0.5f);

    draw->PathLineTo(ImVec2(base + ARROW_HEAD_LENGTH - 1, from));
    draw->PathLineTo(ImVec2(x + radius, from));
    draw->PathBezierQuadraticCurveTo(ImVec2(x, from), ImVec2(x, from + dir * radius), 4);
    if (a.toVisible) {
      // The slant starts where the lane turns: as high above the target as
      // the slope asks, unless the arrow is too short to give it that.
      const float across = base - x;
      const float down = std::min(across * ARROW_SLOPE, std::max(0.0f, span - radius - 1));
      const float length = std::sqrt(across * across + down * down);
      const ImVec2 along(across / length, dir * down / length);
      const ImVec2 corner(x, to - dir * down);
      const float turn = std::min({radius, down * 0.5f, length * 0.5f});
      draw->PathLineTo(ImVec2(x, corner.y - dir * turn));
      draw->PathBezierQuadraticCurveTo(corner, ImVec2(x + along.x * turn, corner.y + along.y * turn), 4);
      // Into the head's base, with a pixel of overlap so no seam shows.
      draw->PathLineTo(ImVec2(base + along.x, to + along.y));
      draw->PathStroke(colour, ImDrawFlags_None, thick);
      const std::pair<int, int> key{static_cast<int>(to), dir > 0 ? 1 : 0};
      const int rank = a.fromPC ? 2 : a.hot ? 1 : 0;
      auto it = arrowHeads.find(key);
      if (it == arrowHeads.end() || rank >= it->second.rank) arrowHeads[key] = {colour, rank, {base, to}, along};
    } else {
      // Off the edge: the line runs to it and a chevron says which way.
      draw->PathLineTo(ImVec2(x, to - dir * 5));
      draw->PathStroke(colour, ImDrawFlags_None, thick);
      draw->AddTriangleFilled(ImVec2(x, to), ImVec2(x - ARROW_HEAD_HALF, to - dir * 6),
                              ImVec2(x + ARROW_HEAD_HALF, to - dir * 6), colour);
    }
  }
  for (const auto &[key, head] : arrowHeads) {
    const ImVec2 u = head.along;
    const ImVec2 n(-u.y, u.x);
    const ImVec2 b = head.at;
    draw->AddTriangleFilled(ImVec2(b.x + u.x * ARROW_HEAD_LENGTH, b.y + u.y * ARROW_HEAD_LENGTH),
                            ImVec2(b.x + n.x * ARROW_HEAD_HALF, b.y + n.y * ARROW_HEAD_HALF),
                            ImVec2(b.x - n.x * ARROW_HEAD_HALF, b.y - n.y * ARROW_HEAD_HALF), head.colour);
  }

  // A selection of more than one line says what it costs, and how much of
  // the profiled time it took.
  if (snapshot_.selection.count > 1) {
    const auto &sel = snapshot_.selection;
    char summary[160];
    int n = std::snprintf(summary, sizeof summary, "%d instructions  \xC2\xB7  ", sel.count);
    if (sel.min == sel.max) n += std::snprintf(summary + n, sizeof summary - n, "%u cycles", sel.min);
    else n += std::snprintf(summary + n, sizeof summary - n, "%u-%u cycles", sel.min, sel.max);
    if (sel.perByte) n += std::snprintf(summary + n, sizeof summary - n, " + 7 a byte moved");
    if (showHeat && heatTotal_) {
      std::snprintf(summary + n, sizeof summary - n, "  \xC2\xB7  %.1f%% of the time", 100.0 * sel.heat / heatTotal_);
    }
    const ImVec2 ts = ImGui::CalcTextSize(summary);
    const ImVec2 b(listRight - 6, end.y - 8);
    const ImVec2 a(b.x - ts.x - 20, b.y - ts.y - 8);
    draw->AddRectFilled(a, b, ui::isDark() ? IM_COL32(30, 30, 36, 240) : IM_COL32(250, 250, 252, 245), (b.y - a.y) * 0.5f);
    draw->AddRect(a, b, accent(0.6f), (b.y - a.y) * 0.5f);
    draw->AddText(ImVec2(a.x + 10, a.y + 4), text(), summary);
  }
  draw->PopClipRect();
  ImGui::PopFont();

  // The scroll so far, in whole lines, moves the top line next frame; what
  // is left over is how far it is drawn above the edge.
  if (count) {
    int moved = 0;
    float offset = scrollPixels_;
    size_t at = top;
    while (offset >= blockHeight[at] && at + 1 < count) {
      offset -= blockHeight[at];
      at++;
      moved++;
    }
    while (offset < 0 && at > 0) {
      at--;
      offset += blockHeight[at];
      moved--;
    }
    if (offset < 0) offset = 0; // the top of the bank
    scrollBy_ += moved;
    scrollPixels_ = offset;
  }

  drawScrollbar(origin, avail);

  // F9 puts a breakpoint on the selected line, as most debuggers do.
  if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && selected_ && ImGui::IsKeyPressed(ImGuiKey_F9, false)) {
    breakpoints_.toggleExec(*selected_);
    ImGui::MarkIniSettingsDirty();
  }
  drawLineMenu();
  ImGui::EndChild();
}

// A scrollbar over the whole bank, since a listing has no length of its own:
// the thumb is where the window is in the 64K, a drag goes anywhere in it,
// and the PC, breakpoints and bookmarks are marked along the track.
void CpuDebugger::drawScrollbar(ImVec2 origin, ImVec2 size) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const Palette p = palette();
  const float x0 = origin.x + size.x - SCROLLBAR_WIDTH - 3;
  const float x1 = x0 + SCROLLBAR_WIDTH;
  const float y0 = origin.y + 6;
  const float y1 = origin.y + size.y - 6;
  const float track = y1 - y0;
  const uint32_t bank = viewTop_ & 0xFF0000;
  constexpr float SPAN = 65536.0f;

  uint32_t shown = 256;
  const auto &lines = snapshot_.lines;
  const size_t top = snapshot_.topIndex;
  const size_t bottom = std::min(lines.size(), top + static_cast<size_t>(visibleRows_));
  if (bottom > top + 1) shown = lines[bottom - 1].address + lines[bottom - 1].length - lines[top].address;
  const float thumbHeight = std::max(28.0f, shown / SPAN * track);
  const float thumbY = y0 + std::min((viewTop_ & 0xFFFF) / SPAN * track, track - thumbHeight);

  ImGui::SetCursorScreenPos(ImVec2(x0 - 2, y0));
  ImGui::InvisibleButton("##scrollbar", ImVec2(x1 - x0 + 4, track));
  const bool hovered = ImGui::IsItemHovered();
  const float my = ImGui::GetIO().MousePos.y;
  if (ImGui::IsItemActivated()) {
    draggingBar_ = true;
    const bool onThumb = my >= thumbY && my <= thumbY + thumbHeight;
    barGrab_ = onThumb ? my - thumbY : thumbHeight * 0.5f;
  }
  uint32_t dragged = viewTop_;
  if (ImGui::IsItemActive() && draggingBar_) {
    const float f = std::clamp((my - barGrab_ - y0) / track, 0.0f, 1.0f);
    dragged = bank | std::min<uint32_t>(static_cast<uint32_t>(f * SPAN), 0xFFFF);
    if (dragged != viewTop_) {
      jumpTo_ = dragged;
      followPC_ = false;
      scrollPixels_ = 0;
    }
    ImGui::SetTooltip("%s", formatAddress(dragged).c_str());
  }
  if (ImGui::IsItemDeactivated()) draggingBar_ = false;

  draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), text(hovered || draggingBar_ ? 0.06f : 0.03f), SCROLLBAR_WIDTH * 0.5f);
  auto mark = [&](uint32_t address, ImU32 colour, float inset) {
    if ((address & 0xFF0000) != bank) return;
    const float y = y0 + (address & 0xFFFF) / SPAN * track;
    draw->AddLine(ImVec2(x0 + inset, y), ImVec2(x1 - inset, y), colour, 2.0f);
  };
  for (const Breakpoint &b : breakpoints_.all()) {
    if (b.kind == Breakpoint::Kind::Exec && b.enabled) mark(b.start, p.red, 1);
  }
  for (uint32_t address : bookmarks_) mark(address, p.blue, 1);
  draw->AddRectFilled(ImVec2(x0 + 1, thumbY), ImVec2(x1 - 1, thumbY + thumbHeight),
                      text(draggingBar_ ? 0.45f : hovered ? 0.32f : 0.2f), (SCROLLBAR_WIDTH - 2) * 0.5f);
  mark(snapshot_.cpu.pc, accent(), 0);
}

void CpuDebugger::drawLineMenu() {
  if (!ImGui::BeginPopup("##linemenu")) return;
  const uint32_t at = menuAddress_;
  const std::string where = formatAddress(at);
  ImGui::TextDisabled("%s", where.c_str());
  ImGui::Separator();
  if (ImGui::MenuItem("Run to Here")) runTo(at);
  if (ImGui::MenuItem("Set PC Here", nullptr, false, snapshot_.paused)) setPC(at);
  ImGui::Separator();
  if (ImGui::MenuItem(breakpoints_.hasExecAt(at) ? "Remove Breakpoint" : "Add Breakpoint", "F9")) {
    breakpoints_.toggleExec(at);
    ImGui::MarkIniSettingsDirty();
  }
  host::Instruction in;
  for (const host::Instruction &line : snapshot_.lines) {
    if (line.address == at) in = line;
  }
  if (ImGui::MenuItem(isBookmarked(at) ? "Remove Bookmark" : "Add Bookmark", "\xE2\x8C\x98-click")) toggleBookmark(at);
  if (in.flow == FlowType::CALL || in.flow == FlowType::UNCONDITIONAL || in.flow == FlowType::CONDITIONAL) {
    if (ImGui::MenuItem("Go to Target")) navigate(in.target);
  }
  ImGui::Separator();
  const DebugSymbols::Label *l = symbols_.label(at);
  if (ImGui::MenuItem(l && !l->name.empty() ? "Rename Label\xE2\x80\xA6" : "Add Label\xE2\x80\xA6")) {
    editKind_ = EditKind::Label;
    editAddress_ = at;
    std::snprintf(editText_, sizeof editText_, "%s", l ? l->name.c_str() : "");
    openEdit_ = true;
  }
  if (ImGui::MenuItem(l && !l->comment.empty() ? "Edit Comment\xE2\x80\xA6" : "Add Comment\xE2\x80\xA6")) {
    editKind_ = EditKind::Comment;
    editAddress_ = at;
    std::snprintf(editText_, sizeof editText_, "%s", l ? l->comment.c_str() : "");
    openEdit_ = true;
  }
  if (ImGui::MenuItem("Copy Address")) ImGui::SetClipboardText(where.c_str());
  ImGui::EndPopup();
}

void CpuDebugger::drawEditPopup() {
  const char *title = editKind_ == EditKind::Label ? "Label" : "Comment";
  if (openEdit_) {
    ImGui::OpenPopup("##editlabel");
    openEdit_ = false;
  }
  ImGui::SetNextWindowSize(ImVec2(360, 0));
  if (!ImGui::BeginPopupModal("##editlabel", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
    return;
  }
  ImGui::Text("%s for %s", title, formatAddress(editAddress_).c_str());
  ImGui::Spacing();
  ImGui::SetNextItemWidth(-1);
  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  const bool enter = ImGui::InputText("##text", editText_, sizeof editText_, ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::Spacing();
  if (ui::Button("Save", ImVec2(90, 0), ui::ButtonKind::Primary) || enter) {
    if (editKind_ == EditKind::Label) symbols_.setLabel(editAddress_, editText_);
    else symbols_.setComment(editAddress_, editText_);
    ImGui::MarkIniSettingsDirty();
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void CpuDebugger::openRuleBuilder(size_t index) {
  if (index >= breakpoints_.all().size()) return;
  ruleTarget_ = breakpoints_.all()[index].id;
  const Breakpoint &b = breakpoints_.all()[index];
  std::string what = formatAddress(b.start) + (b.isRange() ? "-" + formatAddress(b.end) : "");
  if (b.kind == Breakpoint::Kind::Stack) {
    char sp[32];
    std::snprintf(sp, sizeof sp, b.isRange() ? "SP $%X-$%X" : "SP $%X", b.start, b.end);
    what = sp;
  } else if (b.kind == Breakpoint::Kind::Switch) {
    what = Breakpoints::describeSwitch(b, switches_);
  }
  rules_.open(std::string("Condition for ") + KIND_BADGES[static_cast<int>(b.kind)] + " " + what, b.condition);
}

// The builder over the breakpoint it was opened for; Apply writes its
// condition.
void CpuDebugger::drawRuleBuilder() {
  const auto resolve = [this](const std::string &t) { return symbols_.resolve(t, addressMask()); };
  const std::optional<std::string> applied = rules_.draw(resolve, wide_);
  if (!applied) return;
  // By id: a breakpoint deleted elsewhere while the builder was open takes
  // the condition with it rather than handing it to the one after.
  if (Breakpoint *b = breakpoints_.find(ruleTarget_)) {
    b->condition = *applied;
    std::snprintf(conditionText_[b->id].data(), conditionText_[b->id].size(), "%s", applied->c_str());
    ImGui::MarkIniSettingsDirty();
  }
  ruleTarget_ = 0;
}

void CpuDebugger::drawBreakpoints() {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const Palette p = palette();
  // The form: what kind, and where.
  ui::SegmentedControl("##kind", &newKind_, {"Exec", "Read", "Write", "R/W", "SP"}, 260);
  ImGui::SameLine(0, 8);
  // The field takes what the row has left once Add is placed, so a narrow
  // window does not push the button off the panel.
  ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x - 78.0f, 100.0f, 320.0f));
  if (newAddressBad_) ImGui::PushStyleColor(ImGuiCol_FrameBg, withAlpha(p.red, 0.18f));
  // A 65816's stack pointer is sixteen bits, and in emulation mode it lives
  // in page one, so the hint says so.
  const char *hint = newKind_ == 4 ? (wide_ ? "$01E0-$01FF" : "$F0-$FF") : "$2000, $2000-$20FF or COUT";
  bool add = ImGui::InputTextWithHint("##address", hint, newAddress_, sizeof newAddress_, ImGuiInputTextFlags_EnterReturnsTrue);
  if (newAddressBad_) ImGui::PopStyleColor();
  if (ImGui::IsItemEdited()) newAddressBad_ = false;
  ImGui::SameLine(0, 8);
  add = ui::Button("Add", ImVec2(70, 0), ui::ButtonKind::Primary) || add;
  if (add) {
    const uint32_t mask = newKind_ == 4 ? (wide_ ? 0xFFFF : 0xFF) : addressMask();
    if (auto range = Breakpoints::parseRange(newAddress_, symbols_, mask)) {
      Breakpoint b;
      b.kind = static_cast<Breakpoint::Kind>(newKind_);
      b.start = range->first;
      b.end = range->second;
      if (breakpoints_.add(b)) {
        ImGui::MarkIniSettingsDirty();
      }
      newAddress_[0] = 0;
      newAddressBad_ = false;
    } else {
      newAddressBad_ = true;
    }
  }
  ImGui::Spacing();

  // The beam's are in their own tab.
  if (static_cast<int>(breakpoints_.all().size()) == beamCount()) {
    ImGui::TextDisabled("No breakpoints. Click in the gutter beside a line, or add one here.");
    return;
  }
  int removeAt = -1;
  bool changed = false;
  ImGui::BeginChild("##bplist", ImVec2(0, 0), ImGuiChildFlags_None);
  for (size_t i = 0; i < breakpoints_.all().size(); i++) {
    Breakpoint &b = breakpoints_.all()[i];
    if (b.kind == Breakpoint::Kind::Beam) continue;
    // The list is shared, so it may have changed under this window: the
    // condition is read back from it, except while it is being typed.
    std::array<char, 512> &conditionText = conditionText_[b.id];
    if (b.id != editingCondition_) {
      std::snprintf(conditionText.data(), conditionText.size(), "%s", b.condition.c_str());
    }
    ImGui::PushID(static_cast<int>(b.id));
    const ImVec2 rowA = ImGui::GetCursorScreenPos();
    const float rowHeight = ImGui::GetFrameHeight() + 4;
    const float width = ImGui::GetContentRegionAvail().x;
    const bool isHit = b.id == hitId_ && snapshot_.paused;
    if (isHit) draw->AddRectFilled(rowA, ImVec2(rowA.x + width, rowA.y + rowHeight), withAlpha(p.red, 0.14f), 6.0f);
    else if (i % 2) draw->AddRectFilled(rowA, ImVec2(rowA.x + width, rowA.y + rowHeight), text(0.025f), 6.0f);
    ImGui::SetCursorScreenPos(ImVec2(rowA.x + 6, rowA.y + 2));
    if (ui::Checkbox("##on", &b.enabled)) changed = true;
    ImGui::SameLine(0, 8);
    const ImVec2 badge = ImGui::GetCursorScreenPos();
    const float bw = pill(draw, ImVec2(badge.x, badge.y + 3), KIND_BADGES[static_cast<int>(b.kind)], b.enabled,
                          kindColour(b.kind, p), ui::SMALL_TEXT);
    ImGui::Dummy(ImVec2(std::max(bw, 52.0f), 1));
    ImGui::SameLine(0, 8);
    std::string where = formatAddress(b.start);
    if (b.isRange()) where += "-" + formatAddress(b.end);
    if (b.kind == Breakpoint::Kind::Stack) {
      char sp[32];
      std::snprintf(sp, sizeof sp, b.isRange() ? "SP $%X-$%X" : "SP $%X", b.start, b.end);
      where = sp;
    } else if (b.kind == Breakpoint::Kind::Switch) {
      where = Breakpoints::describeSwitch(b, switches_);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(ui::monoFont(), 0.0f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(b.enabled ? text() : secondary()), "%s", where.c_str());
    ImGui::PopFont();
    if (auto sym = symbols_.lookup(b.start); sym && b.isAddress()) {
      ImGui::SameLine(0, 6);
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(symbolColour(sym->category, p)), "%s", sym->name.c_str());
    }
    // The condition, typed straight into the row.
    const float right = rowA.x + width;
    ImGui::SameLine(rowA.x - ImGui::GetWindowPos().x + width * 0.5f);
    ImGui::SetNextItemWidth(right - ImGui::GetCursorScreenPos().x - 180);
    // A condition the evaluator cannot read is marked as it is typed, and
    // says why on hover, rather than quietly never stopping the machine.
    const std::string conditionProblem = b.condition.empty() ? "" : ConditionEvaluator::check(b.condition.c_str());
    if (!conditionProblem.empty()) ImGui::PushStyleColor(ImGuiCol_FrameBg, withAlpha(p.red, 0.18f));
    if (ImGui::InputTextWithHint("##cond", "condition, e.g. A == $41", conditionText.data(), conditionText.size())) {
      b.condition = conditionText.data();
      ImGui::MarkIniSettingsDirty();
    }
    if (!conditionProblem.empty()) ImGui::PopStyleColor();
    if (ImGui::IsItemActive()) editingCondition_ = b.id;
    else if (editingCondition_ == b.id) editingCondition_ = 0;
    if (ImGui::IsItemHovered() && !conditionProblem.empty()) {
      ImGui::SetTooltip("%s", conditionProblem.c_str());
    } else if (ImGui::IsItemHovered()) {
      // A condition the builder can read is said in words; any other is
      // explained.
      const auto tree = b.condition.empty() ? std::nullopt : fromExpression(b.condition);
      if (tree && !tree->children.empty()) {
        ImGui::SetTooltip("Stops only when %s", describe(*tree).c_str());
      } else {
        ImGui::SetTooltip("Stops only when this is true: registers A X Y SP PC, flags,\n"
                          "PEEK($addr), DEEK($addr), == != < > <= >= && || and $hex");
      }
    }
    ImGui::SameLine(0, 6);
    if (ui::Button("Rules\xE2\x80\xA6", ImVec2(64, 0))) openRuleBuilder(i);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Build the condition from rules");
    ImGui::SameLine(0, 8);
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
    char hits[24];
    std::snprintf(hits, sizeof hits, "%u hit%s", b.hits, b.hits == 1 ? "" : "s");
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(b.hits ? text(0.8f) : secondary()), "%s", hits);
    ImGui::PopFont();
    ImGui::SameLine(right - ImGui::GetWindowPos().x - ImGui::GetTextLineHeight() - 6);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f);
    if (removeButton("##remove")) removeAt = static_cast<int>(i);
    ImGui::SetCursorScreenPos(ImVec2(rowA.x, rowA.y + rowHeight + 2));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
  }
  ImGui::EndChild();
  if (removeAt >= 0) {
    conditionText_.erase(breakpoints_.all()[static_cast<size_t>(removeAt)].id);
    breakpoints_.remove(static_cast<size_t>(removeAt));
    editingCondition_ = 0;
    changed = true;
  }
  if (changed) {
    ImGui::MarkIniSettingsDirty();
  }
}

void CpuDebugger::drawWatches() {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const Palette p = palette();
  ImGui::SetNextItemWidth(std::clamp(ImGui::GetContentRegionAvail().x - 78.0f, 120.0f, 420.0f));
  bool add = ImGui::InputTextWithHint("##watch", "A, X, PEEK($24), DEEK($36), PEEK($C000) & $7F", newWatch_,
                                      sizeof newWatch_, ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine(0, 8);
  add = ui::Button("Add", ImVec2(70, 0), ui::ButtonKind::Primary) || add;
  if (add && newWatch_[0]) {
    watches_.push_back({newWatch_, 0, false});
    newWatch_[0] = 0;
    ImGui::MarkIniSettingsDirty();
  }
  ImGui::Spacing();
  if (watches_.empty()) {
    ImGui::TextDisabled("Watch any expression the breakpoint conditions understand; values that change light up at each stop.");
    return;
  }
  int removeAt = -1;
  ImGui::BeginChild("##watchlist", ImVec2(0, 0), ImGuiChildFlags_None);
  for (size_t i = 0; i < watches_.size(); i++) {
    ImGui::PushID(static_cast<int>(i));
    const ImVec2 rowA = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float rowHeight = ImGui::GetTextLineHeight() + 10;
    const bool changed = watches_[i].changed && snapshot_.paused;
    if (changed) draw->AddRectFilled(rowA, ImVec2(rowA.x + width, rowA.y + rowHeight), accent(0.12f), 6.0f);
    else if (i % 2) draw->AddRectFilled(rowA, ImVec2(rowA.x + width, rowA.y + rowHeight), text(0.025f), 6.0f);
    const float ty = rowA.y + 5;
    ImGui::PushFont(ui::monoFont(), 0.0f);
    draw->AddText(ImVec2(rowA.x + 10, ty), text(), watches_[i].expression.c_str());
    const int32_t value = i < snapshot_.watchValues.size() ? snapshot_.watchValues[i] : 0;
    char hex[24], dec[24];
    std::snprintf(hex, sizeof hex, value > 0xFF || value < 0 ? "$%04X" : "$%02X", static_cast<uint32_t>(value) & 0xFFFF);
    std::snprintf(dec, sizeof dec, "%d", value);
    const float vx = rowA.x + width * 0.5f;
    draw->AddText(ImVec2(vx, ty), changed ? ui::accentText() : p.green, hex);
    draw->AddText(ImVec2(vx + 80, ty), secondary(), dec);
    ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(rowA.x + width - ImGui::GetTextLineHeight() - 8, ty));
    if (removeButton("##remove")) removeAt = static_cast<int>(i);
    ImGui::SetCursorScreenPos(ImVec2(rowA.x, rowA.y + rowHeight + 2));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
  }
  ImGui::EndChild();
  if (removeAt >= 0) {
    watches_.erase(watches_.begin() + removeAt);
    ImGui::MarkIniSettingsDirty();
  }
}

void CpuDebugger::drawBeams() {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const Palette p = palette();
  const MachineTiming &t = profile_->timing;
  ui::SegmentedControl("##beammode", &newBeamMode_, {"VBL", "HBL", "Line", "Column", "Line + Col"}, 330);
  ImGui::SameLine(0, 8);
  if (newBeamMode_ == 2 || newBeamMode_ == 4) {
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("##line", &newBeamLine_, 1, 8);
    newBeamLine_ = std::clamp(newBeamLine_, 0, t.scanlinesPerFrame - 1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Scanline, 0 to %d", t.scanlinesPerFrame - 1);
    ImGui::SameLine(0, 8);
  }
  if (newBeamMode_ == 3 || newBeamMode_ == 4) {
    ImGui::SetNextItemWidth(90);
    ImGui::InputInt("##col", &newBeamColumn_, 1, 4);
    newBeamColumn_ = std::clamp(newBeamColumn_, 0, t.visibleColumns - 1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Visible column, 0 to %d", t.visibleColumns - 1);
    ImGui::SameLine(0, 8);
  }
  if (ui::Button("Add", ImVec2(70, 0), ui::ButtonKind::Primary)) {
    Breakpoint beam;
    beam.kind = Breakpoint::Kind::Beam;
    beam.beamMode = newBeamMode_;
    switch (newBeamMode_) {
    case 0: beam.scanline = t.visibleScanlines; beam.hPos = 0; break;
    case 1: beam.scanline = -1; beam.hPos = 0; break;
    case 2: beam.scanline = newBeamLine_; beam.hPos = -1; break;
    case 3: beam.scanline = -1; beam.hPos = newBeamColumn_ + t.hblankCycles; break;
    case 4: beam.scanline = newBeamLine_; beam.hPos = newBeamColumn_ + t.hblankCycles; break;
    }
    if (beamCount() < static_cast<int>(MachineDebug::MAX_BEAM_BREAKPOINTS) && breakpoints_.add(beam)) {
      ImGui::MarkIniSettingsDirty();
    }
  }
  ImGui::Spacing();
  if (beamCount() == 0) {
    ImGui::TextDisabled("Stop when the beam reaches a line, a column, or the start of a blanking interval.");
    return;
  }
  int removeAt = -1;
  bool changed = false;
  ImGui::BeginChild("##beamlist", ImVec2(0, 0), ImGuiChildFlags_None);
  for (size_t i = 0; i < breakpoints_.all().size(); i++) {
    Breakpoint &beam = breakpoints_.all()[i];
    if (beam.kind != Breakpoint::Kind::Beam) continue;
    ImGui::PushID(static_cast<int>(i));
    const ImVec2 rowA = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float rowHeight = ImGui::GetFrameHeight() + 4;
    if (beam.id == beamHitId_ && snapshot_.paused) {
      draw->AddRectFilled(rowA, ImVec2(rowA.x + width, rowA.y + rowHeight), withAlpha(p.red, 0.14f), 6.0f);
    }
    ImGui::SetCursorScreenPos(ImVec2(rowA.x + 6, rowA.y + 2));
    if (ui::Checkbox("##on", &beam.enabled)) changed = true;
    ImGui::SameLine(0, 8);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float bw = pill(draw, ImVec2(at.x, at.y + 3), BEAM_LABELS[beam.beamMode], beam.enabled, p.green, ui::SMALL_TEXT);
    ImGui::Dummy(ImVec2(std::max(bw, 80.0f), 1));
    ImGui::SameLine(0, 8);
    char detail[64];
    switch (beam.beamMode) {
    case 0: std::snprintf(detail, sizeof detail, "Line %d, as vertical blanking starts", beam.scanline); break;
    case 1: std::snprintf(detail, sizeof detail, "Every line, as horizontal blanking starts"); break;
    case 2: std::snprintf(detail, sizeof detail, "Line %d", beam.scanline); break;
    case 3: std::snprintf(detail, sizeof detail, "Column %d, every line", beam.hPos - t.hblankCycles); break;
    default: std::snprintf(detail, sizeof detail, "Line %d, column %d", beam.scanline, beam.hPos - t.hblankCycles); break;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(detail);
    ImGui::SameLine(rowA.x - ImGui::GetWindowPos().x + width - ImGui::GetTextLineHeight() - 6);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f);
    if (removeButton("##remove")) removeAt = static_cast<int>(i);
    ImGui::SetCursorScreenPos(ImVec2(rowA.x, rowA.y + rowHeight + 2));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
  }
  ImGui::EndChild();
  if (removeAt >= 0) {
    breakpoints_.remove(static_cast<size_t>(removeAt));
    changed = true;
  }
  if (changed) ImGui::MarkIniSettingsDirty();
}

void CpuDebugger::drawTrace() {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const Palette p = palette();
  bool recording = snapshot_.traceEnabled;
  if (ui::Switch("Record", &recording)) {
    emulation_.withMachine([&](host::MachineHost &host) {
      if (MachineDebug *debug = host.debug()) debug->setTraceEnabled(recording);
    });
  }
  ImGui::SameLine(0, 16);
  if (ui::Button("Clear")) {
    emulation_.withMachine([](host::MachineHost &host) {
      if (MachineDebug *debug = host.debug()) debug->clearTrace();
    });
  }
  ImGui::SameLine(0, 12);
  ImGui::AlignTextToFramePadding();
  ImGui::TextDisabled("%s instructions, the newest last", grouped(snapshot_.traceCount).c_str());
  ImGui::Spacing();

  const size_t count = std::min<size_t>(snapshot_.traceCount, TRACE_ROWS_MAX);
  const size_t first = snapshot_.traceCount - count;
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.92f);
  const float charWidth = ImGui::CalcTextSize("0").x;
  ImGui::BeginChild("##trace", ImVec2(0, 0), ImGuiChildFlags_None);
  const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2;
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(count));
  while (clipper.Step()) {
    std::vector<host::TraceLine> rows;
    emulation_.withMachine([&](host::MachineHost &host) {
      rows = host.traceLines(first + static_cast<size_t>(clipper.DisplayStart),
                             static_cast<size_t>(clipper.DisplayEnd - clipper.DisplayStart));
    });
    for (size_t r = 0; r < rows.size(); r++) {
      const host::TraceLine &line = rows[r];
      const host::Instruction &in = line.instruction;
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const int digits = wide_ ? 4 : 2;
      float x = at.x + 4;
      char col[48];
      std::snprintf(col, sizeof col, "%10u", line.cycle);
      draw->AddText(ImVec2(x, at.y), text(0.4f), col);
      x += charWidth * 12;
      const std::string address = formatAddress(in.address);
      draw->AddText(ImVec2(x, at.y), secondary(), address.c_str());
      x += charWidth * (wide_ ? 9 : 6);
      draw->AddText(ImVec2(x, at.y), categoryColour(in.category, p), in.mnemonic.c_str());
      x += charWidth * 5;
      const std::string operand = symbolised(in);
      draw->AddText(ImVec2(x, at.y), operand != in.operand ? p.blue : text(), operand.c_str());
      x += charWidth * 20;
      std::snprintf(col, sizeof col, "A=%0*X X=%0*X Y=%0*X SP=%0*X", digits, line.a, digits, line.x, digits, line.y,
                    digits, line.sp);
      draw->AddText(ImVec2(x, at.y), text(0.75f), col);
      x += ImGui::CalcTextSize(col).x + charWidth * 2;
      char flags[9];
      const char *names = wide_ && !(line.widths & MachineDebug::WIDTH_EMULATION) ? "NVMXDIZC" : "NV-BDIZC";
      for (int b = 0; b < 8; b++) flags[b] = (line.p & (0x80 >> b)) ? names[b] : static_cast<char>(std::tolower(names[b]));
      flags[8] = 0;
      draw->AddText(ImVec2(x, at.y), text(0.6f), flags);
      ImGui::Dummy(ImVec2(x + charWidth * 9 - at.x, ImGui::GetTextLineHeight()));
    }
  }
  // Recording, the newest line stays in view unless the user scrolled up.
  if (snapshot_.traceEnabled && atBottom) ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
  ImGui::PopFont();
}

void CpuDebugger::drawPanel(float height) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 a = ImGui::GetCursorScreenPos();
  const float width = ImGui::GetContentRegionAvail().x;
  card(draw, a, ImVec2(a.x + width, a.y + height));
  ImGui::SetCursorScreenPos(ImVec2(a.x + 8, a.y + 4));
  const int beams = beamCount();
  const int counts[] = {static_cast<int>(breakpoints_.all().size()) - beams, static_cast<int>(watches_.size()),
                        beams, -1};
  const char *names[] = {"Breakpoints", "Watch", "Beam", "Trace"};
  const bool flashes[] = {hitId_ && breakpoints_.indexOf(hitId_) >= 0 && snapshot_.paused, false,
                          beamHitId_ && breakpoints_.indexOf(beamHitId_) >= 0 && snapshot_.paused, false};
  for (int i = 0; i < 4; i++) {
    if (panelTab(names[i], counts[i], tab_ == i && !panelFolded_, flashes[i] && tab_ != i)) {
      if (panelFolded_) panelFolded_ = false;
      tab_ = i;
      ImGui::MarkIniSettingsDirty();
    }
  }
  // The fold: a chevron at the end of the bar.
  const float barHeight = ImGui::GetFrameHeight() + 4;
  const ImVec2 fold(a.x + width - barHeight - 4, a.y + 4);
  ImGui::SetCursorScreenPos(fold);
  if (ImGui::InvisibleButton("##fold", ImVec2(barHeight, barHeight))) {
    panelFolded_ = !panelFolded_;
    ImGui::MarkIniSettingsDirty();
  }
  if (ImGui::IsItemHovered()) {
    draw->AddRectFilled(fold, ImVec2(fold.x + barHeight, fold.y + barHeight), text(0.06f), 6.0f);
    ImGui::SetTooltip(panelFolded_ ? "Show the panel" : "Fold the panel away");
  }
  const ImVec2 c(fold.x + barHeight * 0.5f, fold.y + barHeight * 0.5f);
  const float s = 4.5f;
  const float dir = panelFolded_ ? -1.0f : 1.0f;
  draw->AddLine(ImVec2(c.x - s, c.y - s * 0.5f * dir), ImVec2(c.x, c.y + s * 0.5f * dir), secondary(), 1.8f);
  draw->AddLine(ImVec2(c.x, c.y + s * 0.5f * dir), ImVec2(c.x + s, c.y - s * 0.5f * dir), secondary(), 1.8f);
  if (panelFolded_) return;

  draw->AddLine(ImVec2(a.x + 1, a.y + barHeight + 7), ImVec2(a.x + width - 1, a.y + barHeight + 7),
                ImGui::GetColorU32(ImGuiCol_Separator));
  ImGui::SetCursorScreenPos(ImVec2(a.x + PAD, a.y + barHeight + 14));
  ImGui::BeginChild("##panelbody", ImVec2(width - PAD * 2, height - barHeight - 22), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoBackground);
  switch (tab_) {
  case 0: drawBreakpoints(); break;
  case 1: drawWatches(); break;
  case 2: drawBeams(); break;
  default: drawTrace(); break;
  }
  ImGui::EndChild();
}

void CpuDebugger::draw(bool *open) {
  if (!open || !*open || !profile_) return;
  ui::BeforeWindow("CPU Debugger");
  ImGui::SetNextWindowSize(ImVec2(1120, 780), ImGuiCond_FirstUseEver);
  // Tall enough that the column's cards and a few stack entries always fit,
  // and wide enough for the beam form's widest row beside the column.
  ImGui::SetNextWindowSizeConstraints(ImVec2(960, wide_ ? 640.0f : 580.0f), ImVec2(FLT_MAX, FLT_MAX));
  if (!ui::BeginWindow("CPU Debugger", open, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
    ImGui::End();
    return;
  }
  if (!snapshot_.valid) {
    ImGui::TextDisabled("The machine is not running.");
    ImGui::End();
    return;
  }

  drawToolbar();
  ImGui::Dummy(ImVec2(0, 4));

  const ImVec2 avail = ImGui::GetContentRegionAvail();
  drawSidebar(SIDEBAR_WIDTH);
  ImGui::SameLine(0, 10);

  ImGui::BeginGroup();
  const float rightWidth = avail.x - SIDEBAR_WIDTH - 10;
  ImGui::PushItemWidth(rightWidth);
  drawCodeHeader();
  ImGui::PopItemWidth();
  ImGui::Dummy(ImVec2(0, 2));
  const float barHeight = ImGui::GetFrameHeight() + 12;
  const float remaining = ImGui::GetContentRegionAvail().y;
  const float panel = panelFolded_ ? barHeight : std::clamp(panelHeight_, PANEL_MIN, remaining - CODE_MIN);
  drawCode(ImVec2(rightWidth, remaining - panel - 10));

  // The splitter between the code and the panel.
  const ImVec2 splitter = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##splitter", ImVec2(rightWidth, 10));
  if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(splitter.x + rightWidth * 0.5f - 20, splitter.y + 3.5f),
                                              ImVec2(splitter.x + rightWidth * 0.5f + 20, splitter.y + 6.5f),
                                              text(0.25f), 1.5f);
  }
  if (ImGui::IsItemActive() && !panelFolded_) {
    panelHeight_ = std::clamp(panelHeight_ - ImGui::GetIO().MouseDelta.y, PANEL_MIN, remaining - CODE_MIN);
    ImGui::MarkIniSettingsDirty();
  }
  ImGui::SetCursorScreenPos(ImVec2(splitter.x, splitter.y + 10));
  drawPanel(panel);
  ImGui::EndGroup();

  drawEditPopup();
  drawRuleBuilder();
  ImGui::End();
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void CpuDebugger::writeSettings(std::string &out) const {
  char line[96];
  std::snprintf(line, sizeof line, "Tab=%d\nPanelHeight=%.0f\nPanelFolded=%d\nHeat=%d\n", tab_, panelHeight_,
                panelFolded_ ? 1 : 0, heatOn_ ? 1 : 0);
  out += line;
  breakpoints_.writeSettings(out);
  for (const Watch &w : watches_) out += "Watch=" + w.expression + "\n";
  for (uint32_t address : bookmarks_) {
    std::snprintf(line, sizeof line, "Bookmark=%06X\n", address);
    out += line;
  }
  symbols_.writeSettings(out);
}

void CpuDebugger::readSetting(const char *line) {
  int value = 0;
  float number = 0;
  if (std::sscanf(line, "Tab=%d", &value) == 1) tab_ = std::clamp(value, 0, 3);
  else if (std::sscanf(line, "PanelHeight=%f", &number) == 1) panelHeight_ = std::max(number, PANEL_MIN);
  else if (std::sscanf(line, "PanelFolded=%d", &value) == 1) panelFolded_ = value;
  else if (std::sscanf(line, "Heat=%d", &value) == 1) heatOn_ = value;
  else if (!std::strncmp(line, "Bookmark=", 9)) {
    const uint32_t address = static_cast<uint32_t>(std::strtoul(line + 9, nullptr, 16)) & 0xFFFFFF;
    if (!isBookmarked(address)) bookmarks_.insert(std::lower_bound(bookmarks_.begin(), bookmarks_.end(), address), address);
  } else if (!std::strncmp(line, "Watch=", 6)) watches_.push_back({line + 6, 0, false});
  else if (!breakpoints_.readSetting(line)) {
    symbols_.readSetting(line);
  }
}

} // namespace a2e::native
