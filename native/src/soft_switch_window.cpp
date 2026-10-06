/*
 * soft_switch_window.cpp - The soft switches, live, with breakpoints on them
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "soft_switch_window.hpp"

#include "emulation.hpp"
#include "ui_controls.hpp"
#include "ui_theme.hpp"

#include "imgui.h"
#include "imgui_internal.h" // MarkIniSettingsDirty

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace a2e::native {

namespace {

constexpr float WIDTH = 520;

ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }

// A group's heading: as large as the rows, not smaller, in the accent colour
// with a rule under it, so the groups can be told apart at a glance by
// someone who does not see small type well.
void heading(const char *label) {
  ImGui::Dummy(ImVec2(0, 6));
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.05f);
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::accentText()), "%s", label);
  ImGui::PopFont();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, at.y - 1),
                                      ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y - 1),
                                      ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
  ImGui::Dummy(ImVec2(0, 3));
}

// A byte as the debugger writes one: "$C1", "C1".
bool parseByte(const char *text, uint8_t &out) {
  while (*text == ' ') text++;
  if (*text == '$') text++;
  if (!*text) return false;
  char *end = nullptr;
  const long value = std::strtol(text, &end, 16);
  if (*end || value < 0 || value > 0xFF) return false;
  out = static_cast<uint8_t>(value);
  return true;
}

} // namespace

SoftSwitchWindow::SoftSwitchWindow(Emulation &emulation, Breakpoints &breakpoints)
    : emulation_(emulation), breakpoints_(breakpoints) {
  std::snprintf(maskText_.data(), maskText_.size(), "FF");
}

void SoftSwitchWindow::setMachine() {
  emulation_.withMachine([&](host::MachineHost &host) { catalog_ = host.softSwitches(); });
  registers_.assign(catalog_.size(), 0);
}

Breakpoint SoftSwitchWindow::spec(const SoftSwitchInfo &sw, bool equals, uint8_t value, uint8_t mask) {
  Breakpoint b;
  b.kind = Breakpoint::Kind::Switch;
  b.key = sw.key;
  b.equals = equals;
  b.value = value;
  b.mask = mask;
  return b;
}

void SoftSwitchWindow::take() {
  emulation_.poll(takePoll_, [&](host::MachineHost &host) {
    flags_ = host.softSwitchValue(MachineDebug::SWITCH_FLAGS);
    for (size_t i = 0; i < catalog_.size() && i < registers_.size(); i++) {
      if (catalog_[i].isRegister()) registers_[i] = static_cast<uint8_t>(host.softSwitchValue(catalog_[i].source));
    }
    MachineDebug *debug = host.debug();
    paused_ = host.isPaused();
    hit_ = paused_ && debug && debug->isSwitchBreakpointHit();
    hitId_ = hit_ ? debug->switchBreakpointHitId() : -1;
    hitText_ = hit_ ? host.switchHitText() : std::string();
  });
}

// The switch breakpoints from the shared list, which the CPU debugger also
// shows among the rest.
void SoftSwitchWindow::drawBreakpoints() {
  std::vector<size_t> rows;
  for (size_t i = 0; i < breakpoints_.all().size(); i++) {
    if (breakpoints_.all()[i].kind == Breakpoint::Kind::Switch) rows.push_back(i);
  }
  if (rows.empty()) return;
  const ImVec2 top = ImGui::GetCursorPos();
  heading("Breakpoints");
  const ImVec2 below = ImGui::GetCursorPos();
  ImGui::SetCursorPos(ImVec2(top.x + WIDTH - ImGui::CalcTextSize("Clear").x - 16, top.y + 4));
  const bool clear = ui::Button("Clear", ImVec2(0, 0), ui::ButtonKind::Normal);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove every soft switch breakpoint");
  ImGui::SetCursorPos(below);
  if (clear) {
    for (size_t r = rows.size(); r-- > 0;) breakpoints_.remove(rows[r]);
    ImGui::MarkIniSettingsDirty();
    return;
  }

  int remove = -1;
  for (size_t i : rows) {
    Breakpoint &bp = breakpoints_.all()[i];
    // By id, so a row keeps its widgets' state when another window deletes
    // a breakpoint above it.
    ImGui::PushID(static_cast<int>(bp.id));
    const bool present = find(bp.key) != nullptr;
    const std::string text = Breakpoints::describeSwitch(bp, catalog_);
    ImGui::BeginDisabled(!present);
    if (ui::Checkbox("##on", &bp.enabled)) ImGui::MarkIniSettingsDirty();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::PushFont(ui::monoFont(), 0.0f);
    if (present) {
      ImGui::TextUnformatted(text.c_str());
    } else {
      ImGui::TextDisabled("%s", text.c_str());
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Not on this machine");
    }
    if (!bp.condition.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("if %s", bp.condition.c_str());
    }
    if (bp.hits) {
      ImGui::SameLine();
      ImGui::TextDisabled("%u hit%s", bp.hits, bp.hits == 1 ? "" : "s");
    }
    ImGui::PopFont();
    ImGui::SameLine(WIDTH - 22);
    if (ImGui::SmallButton("\xC3\x97")) remove = static_cast<int>(i);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove");
    ImGui::PopID();
  }
  if (remove >= 0) {
    breakpoints_.remove(static_cast<size_t>(remove));
    ImGui::MarkIniSettingsDirty();
  }
}

// The breakpoints a switch can have, as a menu off its dot.
void SoftSwitchWindow::drawMenu(const SoftSwitchInfo &sw) {
  if (!ImGui::BeginPopup("##breakpoint")) return;
  auto has = [&](bool equals, uint8_t value) {
    for (const Breakpoint &bp : breakpoints_.all()) {
      if (bp.kind == Breakpoint::Kind::Switch && bp.key == sw.key && bp.equals == equals &&
          (!equals || bp.value == value)) {
        return true;
      }
    }
    return false;
  };
  auto toggle = [&](const Breakpoint &b) {
    breakpoints_.toggle(b);
    ImGui::MarkIniSettingsDirty();
  };

  char label[64];
  std::snprintf(label, sizeof label, "Break when %s changes", sw.name);
  if (ImGui::MenuItem(label, nullptr, has(false, 0))) toggle(spec(sw, false, 0, 0xFF));

  if (!sw.isRegister()) {
    if (ImGui::MenuItem("Break when it turns on", nullptr, has(true, 1))) toggle(spec(sw, true, 1, 0xFF));
    if (ImGui::MenuItem("Break when it turns off", nullptr, has(true, 0))) toggle(spec(sw, true, 0, 0xFF));
  } else {
    // A value under a mask: NEWVIDEO & $80 = $80 is Super Hi-Res coming on,
    // whatever the other bits are doing.
    ImGui::Separator();
    const ImGuiInputTextFlags hex = ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase;
    ImGui::PushFont(ui::monoFont(), 0.0f);
    const float field = ImGui::CalcTextSize("$FF").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::TextUnformatted("Value $");
    ImGui::SameLine(0, 0);
    ImGui::SetNextItemWidth(field);
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    bool enter = ImGui::InputText("##value", valueText_.data(), 3, hex | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::TextUnformatted("Mask $");
    ImGui::SameLine(0, 0);
    ImGui::SetNextItemWidth(field);
    enter |= ImGui::InputText("##mask", maskText_.data(), 3, hex | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopFont();
    ImGui::SameLine();
    uint8_t value = 0, mask = 0;
    const bool valid = parseByte(valueText_.data(), value) && parseByte(maskText_.data(), mask) && mask != 0;
    ImGui::BeginDisabled(!valid);
    if ((ui::Button("Add") || enter) && valid) {
      if (breakpoints_.add(spec(sw, true, value & mask, mask))) ImGui::MarkIniSettingsDirty();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
  }
  ImGui::EndPopup();
}

// One row: the breakpoint dot, the address, the name in a capsule lit while
// the switch is on, a register's value, and what it is. Everything is drawn
// about one centre line, so the capsule and the text beside it line up
// whatever the two fonts' heights are, and nothing is smaller than the
// window's body text.
void SoftSwitchWindow::drawSwitch(const SoftSwitchInfo &sw, float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ui::Palette &p = ui::palette();
  const size_t index = static_cast<size_t>(&sw - catalog_.data());
  ImGui::PushID(sw.key);

  ImGui::PushFont(ui::monoFont(), 0.0f);
  const float monoLine = ImGui::GetTextLineHeight();
  const float addressWidth = ImGui::CalcTextSize("$C080-8F").x;
  const ImVec2 chipText = ImGui::CalcTextSize("SLOTC3ROM");
  const float valueWidth = ImGui::CalcTextSize("$FF").x;
  ImGui::PopFont();
  const float bodyLine = ImGui::GetTextLineHeight();
  const float chipHeight = std::max(monoLine, bodyLine) + 6;
  const float rowHeight = chipHeight + 6;

  const ImVec2 row = ImGui::GetCursorScreenPos();
  const float mid = row.y + rowHeight * 0.5f;

  bool armed = false;
  bool stoppedHere = false;
  for (const Breakpoint &bp : breakpoints_.all()) {
    if (bp.kind != Breakpoint::Kind::Switch || bp.key != sw.key) continue;
    armed |= bp.enabled;
    stoppedHere |= hit_ && bp.coreId == hitId_;
  }
  if (stoppedHere) {
    draw->AddRectFilled(ImVec2(row.x - 4, row.y), ImVec2(row.x + width, row.y + rowHeight),
                        (p.red & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 50), 4.0f);
  }

  // The dot: hollow, or filled red with a breakpoint armed.
  const float dotSize = rowHeight;
  if (ImGui::InvisibleButton("##dot", ImVec2(dotSize, rowHeight))) ImGui::OpenPopup("##breakpoint");
  const bool hovered = ImGui::IsItemHovered();
  if (hovered) ImGui::SetTooltip("Breakpoint on %s", sw.name);
  const ImVec2 centre(row.x + dotSize * 0.5f - 2, mid);
  if (armed) {
    draw->AddCircleFilled(centre, 5.5f, p.red);
  } else {
    draw->AddCircle(centre, 5.5f, hovered ? p.red : secondary(), 0, 1.5f);
  }
  drawMenu(sw);

  float x = row.x + dotSize + 2;
  ImGui::PushFont(ui::monoFont(), 0.0f);
  draw->AddText(ImVec2(x, mid - monoLine * 0.5f), p.orange, sw.address);
  x += addressWidth + 12;

  const bool lit = sw.isRegister() || (flags_ & sw.mask());
  const ImU32 colour = sw.readOnly ? p.blue : p.green;
  const ImVec2 ca(x, mid - chipHeight * 0.5f);
  const ImVec2 cb(x + chipText.x + 16, mid + chipHeight * 0.5f);
  draw->AddRectFilled(ca, cb, lit ? colour : ImGui::GetColorU32(ImGuiCol_Text, 0.08f), 5.0f);
  const float nameWidth = ImGui::CalcTextSize(sw.name).x;
  draw->AddText(ImVec2(ca.x + (cb.x - ca.x - nameWidth) * 0.5f, mid - monoLine * 0.5f),
                lit ? ui::textOn(colour) : secondary(), sw.name);
  x = cb.x + 12;

  if (sw.isRegister()) {
    char value[8];
    std::snprintf(value, sizeof value, "$%02X", index < registers_.size() ? registers_[index] : 0);
    draw->AddText(ImVec2(x, mid - monoLine * 0.5f), p.green, value);
    x += valueWidth + 12;
  }
  ImGui::PopFont();

  draw->AddText(ImVec2(x, mid - bodyLine * 0.5f), ImGui::GetColorU32(ImGuiCol_Text, 0.78f), sw.description);

  ImGui::SetCursorScreenPos(ImVec2(row.x + dotSize, row.y));
  ImGui::Dummy(ImVec2(std::max(1.0f, width - dotSize), rowHeight));
  ImGui::PopID();
}

void SoftSwitchWindow::draw(bool *open) {
  if (!open || !*open) return;
  take();
  ui::BeforeWindow("Soft Switches");
  const ImGuiStyle &style = ImGui::GetStyle();
  const float width = WIDTH + style.WindowPadding.x * 2 + style.ScrollbarSize;
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, 200), ImVec2(FLT_MAX, FLT_MAX));
  ImGui::SetNextWindowSize(ImVec2(width, 560), ImGuiCond_FirstUseEver);
  if (ui::BeginWindow("Soft Switches", open)) {
    if (hit_) {
      ImGui::PushFont(ui::monoFont(), 0.0f);
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::palette().red), "Stopped: %s", hitText_.c_str());
      ImGui::PopFont();
      ImGui::Separator();
    }
    drawBreakpoints();

    const char *group = nullptr;
    for (const SoftSwitchInfo &sw : catalog_) {
      if (!group || std::strcmp(group, sw.group) != 0) {
        group = sw.group;
        heading(group);
      }
      drawSwitch(sw, ImGui::GetContentRegionAvail().x);
    }
  }
  ImGui::End();
}

// The breakpoints are in the shared list and saved with it (the CPU
// debugger's section). A line this window wrote before that is handed over.
void SoftSwitchWindow::writeSettings(std::string &) const {}

void SoftSwitchWindow::readSetting(const char *line) { breakpoints_.readSetting(line); }

} // namespace a2e::native
