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

constexpr float WIDTH = 420;

ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }

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

bool SoftSwitchWindow::SwitchBreak::same(const SwitchBreak &other) const {
  if (key != other.key || condition != other.condition) return false;
  return condition == Condition::Changes || (value == other.value && mask == other.mask);
}

SoftSwitchWindow::SoftSwitchWindow(Emulation &emulation) : emulation_(emulation) {
  std::snprintf(maskText_.data(), maskText_.size(), "FF");
}

const SoftSwitchInfo *SoftSwitchWindow::find(const std::string &key) const {
  return findSoftSwitch(catalog_, key.c_str());
}

std::string SoftSwitchWindow::describe(const SwitchBreak &bp) const {
  const SoftSwitchInfo *sw = find(bp.key);
  std::string name = sw ? sw->name : bp.key;
  if (!sw) std::transform(name.begin(), name.end(), name.begin(), ::toupper);
  char text[64];
  if (bp.condition == Condition::Changes) {
    std::snprintf(text, sizeof text, "%s changes", name.c_str());
  } else if (!sw || !sw->isRegister()) {
    std::snprintf(text, sizeof text, "%s %s", name.c_str(), bp.value ? "on" : "off");
  } else if (bp.mask == 0xFF) {
    std::snprintf(text, sizeof text, "%s = $%02X", name.c_str(), bp.value);
  } else {
    std::snprintf(text, sizeof text, "%s & $%02X = $%02X", name.c_str(), bp.mask, bp.value);
  }
  return text;
}

void SoftSwitchWindow::setMachine() {
  emulation_.withMachine([&](host::MachineHost &host) { catalog_ = host.softSwitches(); });
  registers_.assign(catalog_.size(), 0);
  countedId_ = -1;
  apply();
}

// The core holds exactly the enabled breakpoints on switches this machine
// has, and is handed them afresh after any change.
void SoftSwitchWindow::apply() {
  emulation_.withMachine([&](host::MachineHost &host) {
    MachineDebug *debug = host.debug();
    for (SwitchBreak &bp : breaks_) bp.coreId = -1;
    if (!debug) return;
    debug->clearSwitchBreakpoints();
    for (SwitchBreak &bp : breaks_) {
      const SoftSwitchInfo *sw = find(bp.key);
      if (!bp.enabled || !sw) continue;
      const auto condition = bp.condition == Condition::Changes ? MachineDebug::SwitchCondition::Changes
                                                                : MachineDebug::SwitchCondition::Equals;
      const uint64_t mask = sw->isRegister() ? bp.mask : sw->mask();
      const uint64_t value = sw->isRegister() ? bp.value : (bp.value ? mask : 0);
      bp.coreId = debug->addSwitchBreakpoint(sw->source, mask, condition, value);
    }
  });
  countedId_ = -1;
  ImGui::MarkIniSettingsDirty();
}

void SoftSwitchWindow::add(SwitchBreak bp) {
  for (const SwitchBreak &existing : breaks_) {
    if (existing.same(bp)) return;
  }
  breaks_.push_back(std::move(bp));
  apply();
}

void SoftSwitchWindow::toggle(const SwitchBreak &bp) {
  for (size_t i = 0; i < breaks_.size(); i++) {
    if (breaks_[i].same(bp)) {
      breaks_.erase(breaks_.begin() + static_cast<long>(i));
      apply();
      return;
    }
  }
  add(bp);
}

void SoftSwitchWindow::update() {
  if (breaks_.empty()) return;
  emulation_.poll(updatePoll_, [&](host::MachineHost &host) {
    MachineDebug *debug = host.debug();
    if (!debug || !host.isPaused() || !debug->isSwitchBreakpointHit()) {
      countedId_ = -1;
      return;
    }
    const int32_t id = debug->switchBreakpointHitId();
    if (id == countedId_) return;
    countedId_ = id;
    for (SwitchBreak &bp : breaks_) {
      if (bp.coreId == id) bp.hits++;
    }
  });
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

void SoftSwitchWindow::drawBreakpoints() {
  if (breaks_.empty()) return;
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.72f);
  ImGui::TextDisabled("BREAKPOINTS");
  ImGui::PopFont();
  ImGui::SameLine(WIDTH - ImGui::CalcTextSize("Clear").x - 16);
  if (ui::Button("Clear", ImVec2(0, 0), ui::ButtonKind::Normal)) {
    breaks_.clear();
    apply();
    return;
  }

  int remove = -1;
  bool changed = false;
  for (size_t i = 0; i < breaks_.size(); i++) {
    SwitchBreak &bp = breaks_[i];
    ImGui::PushID(static_cast<int>(i));
    const bool present = find(bp.key) != nullptr;
    ImGui::BeginDisabled(!present);
    if (ui::Checkbox("##on", &bp.enabled)) changed = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::PushFont(ui::monoFont(), 0.0f);
    if (present) {
      ImGui::TextUnformatted(describe(bp).c_str());
    } else {
      ImGui::TextDisabled("%s", describe(bp).c_str());
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Not on this machine");
    }
    if (bp.hits) {
      ImGui::SameLine();
      ImGui::TextDisabled("%u", bp.hits);
    }
    ImGui::PopFont();
    ImGui::SameLine(WIDTH - 22);
    if (ImGui::SmallButton("\xC3\x97")) remove = static_cast<int>(i);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove");
    ImGui::PopID();
  }
  if (remove >= 0) {
    breaks_.erase(breaks_.begin() + remove);
    changed = true;
  }
  if (changed) apply();
  ImGui::Separator();
}

// The breakpoints a switch can have, as a menu off its dot.
void SoftSwitchWindow::drawMenu(const SoftSwitchInfo &sw) {
  if (!ImGui::BeginPopup("##breakpoint")) return;
  auto has = [&](Condition condition, uint8_t value) {
    for (const SwitchBreak &bp : breaks_) {
      if (bp.key == sw.key && bp.condition == condition &&
          (condition == Condition::Changes || bp.value == value)) {
        return true;
      }
    }
    return false;
  };
  auto spec = [&](Condition condition, uint8_t value, uint8_t mask) {
    SwitchBreak bp;
    bp.key = sw.key;
    bp.condition = condition;
    bp.value = value;
    bp.mask = mask;
    return bp;
  };

  char label[64];
  std::snprintf(label, sizeof label, "Break when %s changes", sw.name);
  if (ImGui::MenuItem(label, nullptr, has(Condition::Changes, 0))) toggle(spec(Condition::Changes, 0, 0xFF));

  if (!sw.isRegister()) {
    if (ImGui::MenuItem("Break when it turns on", nullptr, has(Condition::Equals, 1))) {
      toggle(spec(Condition::Equals, 1, 0xFF));
    }
    if (ImGui::MenuItem("Break when it turns off", nullptr, has(Condition::Equals, 0))) {
      toggle(spec(Condition::Equals, 0, 0xFF));
    }
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
      add(spec(Condition::Equals, value & mask, mask));
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
  }
  ImGui::EndPopup();
}

void SoftSwitchWindow::drawSwitch(const SoftSwitchInfo &sw, float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ui::Palette &p = ui::palette();
  const size_t index = static_cast<size_t>(&sw - catalog_.data());
  ImGui::PushID(sw.key);

  // The dot: hollow, or filled red with a breakpoint armed.
  bool armed = false;
  for (const SwitchBreak &bp : breaks_) armed |= bp.key == sw.key && bp.enabled;
  const float line = ImGui::GetTextLineHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool stoppedHere = [&] {
    for (const SwitchBreak &bp : breaks_) {
      if (hit_ && bp.coreId == hitId_ && bp.key == sw.key) return true;
    }
    return false;
  }();
  if (stoppedHere) {
    draw->AddRectFilled(ImVec2(at.x - 4, at.y - 1), ImVec2(at.x + width, at.y + line + 1),
                        (p.red & ~IM_COL32_A_MASK) | IM_COL32(0, 0, 0, 50), 4.0f);
  }
  if (ImGui::InvisibleButton("##dot", ImVec2(line, line))) ImGui::OpenPopup("##breakpoint");
  const bool hovered = ImGui::IsItemHovered();
  if (hovered) ImGui::SetTooltip("Breakpoint on %s", sw.name);
  const ImVec2 centre(at.x + line * 0.5f, at.y + line * 0.5f);
  if (armed) {
    draw->AddCircleFilled(centre, 4.5f, p.red);
  } else {
    draw->AddCircle(centre, 4.5f, hovered ? p.red : secondary(), 0, 1.2f);
  }
  drawMenu(sw);

  // The address, the name lit while on, a register's value, what it is.
  ImGui::SameLine();
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.85f);
  ImGui::AlignTextToFramePadding();
  const float addressAt = ImGui::GetCursorPosX();
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(p.orange), "%s", sw.address);
  ImGui::SameLine(addressAt + ImGui::CalcTextSize("$C080-8F").x + 10);

  const bool lit = sw.isRegister() || (flags_ & sw.mask());
  const ImU32 colour = sw.readOnly ? p.blue : p.green;
  const ImVec2 size = ImGui::CalcTextSize("SLOTC3ROM");
  const ImVec2 pa = ImGui::GetCursorScreenPos();
  const ImVec2 pb(pa.x + size.x + 10, pa.y + size.y + 2);
  draw->AddRectFilled(pa, pb, lit ? colour : ImGui::GetColorU32(ImGuiCol_Text, 0.07f), 4.0f);
  const float nameWidth = ImGui::CalcTextSize(sw.name).x;
  draw->AddText(ImVec2(pa.x + 5 + (size.x - nameWidth) * 0.5f, pa.y + 1), lit ? ui::textOn(colour) : secondary(),
                sw.name);
  ImGui::Dummy(ImVec2(pb.x - pa.x, pb.y - pa.y));
  ImGui::SameLine();
  if (sw.isRegister()) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(p.green), "$%02X",
                       index < registers_.size() ? registers_[index] : 0);
    ImGui::SameLine();
  }
  ImGui::PopFont();
  ImGui::TextDisabled("%s", sw.description);
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
        ImGui::Dummy(ImVec2(0, 4));
        ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.72f);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ui::accentText()), "%s", group);
        ImGui::PopFont();
      }
      drawSwitch(sw, ImGui::GetContentRegionAvail().x);
    }
  }
  ImGui::End();
}

// "SwitchBreakpoint=<key>\t<change|equals>\t<value>\t<mask>\t<enabled>"
void SoftSwitchWindow::writeSettings(std::string &out) const {
  char line[128];
  for (const SwitchBreak &bp : breaks_) {
    std::snprintf(line, sizeof line, "SwitchBreakpoint=%s\t%s\t%u\t%u\t%d\n", bp.key.c_str(),
                  bp.condition == Condition::Changes ? "change" : "equals", bp.value, bp.mask, bp.enabled ? 1 : 0);
    out += line;
  }
}

void SoftSwitchWindow::readSetting(const char *line) {
  char key[32], condition[16];
  unsigned value = 0, mask = 0xFF;
  int enabled = 1;
  if (std::sscanf(line, "SwitchBreakpoint=%31[^\t]\t%15[^\t]\t%u\t%u\t%d", key, condition, &value, &mask,
                  &enabled) != 5) {
    return;
  }
  SwitchBreak bp;
  bp.key = key;
  if (!std::strcmp(condition, "change")) {
    bp.condition = Condition::Changes;
  } else if (!std::strcmp(condition, "equals")) {
    bp.condition = Condition::Equals;
  } else {
    return;
  }
  bp.value = static_cast<uint8_t>(value);
  bp.mask = static_cast<uint8_t>(mask);
  bp.enabled = enabled != 0;
  for (const SwitchBreak &existing : breaks_) {
    if (existing.same(bp)) return;
  }
  // Armed when the machine is next set, which follows reading the settings.
  breaks_.push_back(std::move(bp));
}

} // namespace a2e::native
