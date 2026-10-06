/*
 * save_states.cpp - The Save States window: an autosave and five slots
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "save_states.hpp"
#include "ui_controls.hpp"

#include "emulation.hpp"
#include "media_store.hpp"

#include "machine/machine_profile.hpp"

#include "ui_theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace a2e::native {

namespace {

constexpr const char *SWITCH_POPUP = "Load State";
constexpr const char *ERROR_POPUP = "Save State Error";
constexpr double AUTOSAVE_SECONDS = 5.0;

std::string formatTime(int64_t milliseconds) {
  const std::time_t seconds = static_cast<std::time_t>(milliseconds / 1000);
  std::tm local{};
  localtime_r(&seconds, &local);
  char text[64];
  std::strftime(text, sizeof(text), "%e %b %Y, %H:%M", &local);
  return text;
}

const MachineProfile *profileForId(uint32_t id) {
  for (int i = 0; i < MACHINE_COUNT; i++) {
    const MachineProfile &profile = machineProfileAt(i);
    if (static_cast<uint32_t>(profile.id) == id) return &profile;
  }
  return nullptr;
}

} // namespace

SaveStates::SaveStates(Emulation &emulation, Platform &platform, std::string directory, Hooks hooks)
    : emulation_(emulation), platform_(platform), store_(std::move(directory)), hooks_(std::move(hooks)) {}

SaveStates::~SaveStates() {
  if (backgroundSave_.valid()) backgroundSave_.wait();
  releaseThumbnails();
}

void SaveStates::releaseThumbnails() {
  for (auto &[id, row] : rows_) {
    if (row.thumbnail != ImTextureID_Invalid && platform_.releaseTexture) platform_.releaseTexture(row.thumbnail);
  }
  rows_.clear();
}

// Read the records again: after a save or a clear, and when the window opens.
void SaveStates::refresh() {
  releaseThumbnails();
  const MachineProfile *machine = hooks_.machine();
  std::vector<std::string> ids;
  if (machine) {
    ids.push_back(StateStore::autosaveId(machine->key));
    ids.push_back(StateStore::lastSessionId(machine->key));
  }
  for (int slot = 1; slot <= StateStore::SLOTS; slot++) ids.push_back(StateStore::slotId(slot));
  for (const std::string &id : ids) {
    Row row;
    row.record = store_.info(id);
    if (row.record && !row.record->thumbnail.empty() && platform_.makeTexture) {
      row.thumbnail = platform_.makeTexture(row.record->thumbnail.data(), THUMB_WIDTH, THUMB_HEIGHT);
    }
    rows_[id] = std::move(row);
  }
  stale_ = false;
}

void SaveStates::notice(const std::string &message) {
  notice_ = message;
  noticeUntil_ = ImGui::GetTime() + 3.0;
}

void SaveStates::error(const std::string &message) {
  error_ = message;
  openError_ = true;
}

bool SaveStates::saveTo(const std::string &id, bool background) {
  const MachineProfile *machine = hooks_.machine();
  if (!machine) return false;
  finishBackgroundSave(true);
  std::vector<uint8_t> state;
  std::vector<uint8_t> thumbnail;
  emulation_.withMachine([&](host::MachineHost &host) {
    size_t size = 0;
    const uint8_t *bytes = host.exportState(&size);
    if (bytes && size) state.assign(bytes, bytes + size);
    const auto &display = machine->display;
    if (host.framebuffer() && host.framebufferSize() ==
                                  static_cast<size_t>(display.pixelWidth) * display.pixelHeight * 4) {
      thumbnail = makeThumbnail(host.framebuffer(), display.pixelWidth, display.pixelHeight);
    }
  });
  if (state.empty()) {
    error("The machine could not be saved.");
    return false;
  }
  // The periodic autosave writes its file away from the UI: a machine with
  // two hard drive images is a 72MB state, and writing it here held up a
  // frame every five seconds. The copy above is all that needs the machine.
  if (background) {
    backgroundSave_ = std::async(std::launch::async, [this, id, key = machine->key, state = std::move(state),
                                                      thumbnail = std::move(thumbnail)] {
      return store_.save(id, key, state, thumbnail);
    });
    return true;
  }
  if (!store_.save(id, machine->key, state, thumbnail)) {
    error("The save state could not be written.");
    return false;
  }
  stale_ = true;
  return true;
}

// A save written in the background, collected: waited for when `wait`, as
// before another save or the app going, otherwise only once it is done.
void SaveStates::finishBackgroundSave(bool wait) {
  if (!backgroundSave_.valid()) return;
  if (!wait && backgroundSave_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
  if (!backgroundSave_.get()) error("The autosave could not be written.");
  stale_ = true;
}

void SaveStates::autosaveNow(bool background) {
  if (!autosave || !emulation_.powered() || !hooks_.machine()) return;
  // The autosave the last quit wrote is kept as the last session before this
  // session replaces it: it used to be overwritten five seconds after
  // starting, so a quit's autosave could never be gone back to.
  const std::string &key = hooks_.machine()->key;
  if (keptLastSession_.insert(key).second) {
    store_.copy(StateStore::autosaveId(key), StateStore::lastSessionId(key));
    stale_ = true;
  }
  saveTo(StateStore::autosaveId(key), background);
}

void SaveStates::update(double now) {
  finishBackgroundSave(false);
  if (!autosave || !emulation_.powered()) {
    lastAutosave_ = now;
    return;
  }
  if (now - lastAutosave_ < AUTOSAVE_SECONDS) return;
  // One at a time: a write still going when the next is due lets it pass.
  if (backgroundSave_.valid()) return;
  lastAutosave_ = now;
  autosaveNow(true);
}

// A state names the machine that wrote it. The same machine takes it at
// once; another is asked about first.
bool SaveStates::loadBytes(const std::vector<uint8_t> &data, const std::string &what) {
  const auto header = readStateHeader(data.data(), data.size());
  if (!header) {
    error("That is not an ApplEm save state.");
    return false;
  }
  const MachineProfile *target = profileForId(header->machineId);
  const MachineProfile *current = hooks_.machine();
  if (!target) {
    error("That state was saved on a machine this version does not know.");
    return false;
  }
  if (current && target->id == current->id) return importNow(data, what);
  if (!Emulator::isMachineRunnable(target->id)) {
    error(std::string("That state was saved on the ") + target->name +
          ", whose ROM is not built into this copy of ApplEm.");
    return false;
  }
  pendingLoad_ = data;
  pendingWhat_ = what;
  pendingMachine_ = static_cast<int>(target->id);
  pendingMachineName_ = target->name;
  // Asked first; the answer loads it, or not.
  openSwitch_ = true;
  return false;
}

bool SaveStates::importNow(const std::vector<uint8_t> &data, const std::string &what) {
  if (!emulation_.powered()) emulation_.setPowered(true);
  if (hooks_.loading) hooks_.loading();
  const bool ok = emulation_.withMachine([&](host::MachineHost &host) { return host.importState(data.data(), data.size()); });
  if (!ok) {
    error("The state could not be loaded.");
    return false;
  }
  if (hooks_.loaded) hooks_.loaded();
  notice("Loaded " + what + ".");
  return true;
}

void SaveStates::exportRecord(const std::string &id, const std::string &suggestedName) {
  auto data = store_.load(id);
  if (!data) return;
  std::vector<uint8_t> bytes = std::move(*data);
  platform_.saveFile("Save the state to a file", suggestedName, {"a2state"},
                     [this, bytes](const std::string &path) {
                       if (path.empty()) return;
                       if (!writeFile(path, bytes.data(), bytes.size())) error("Could not write " + path + ".");
                     });
}

namespace {

constexpr float CARD_ROUNDING = 10;
constexpr float COLUMN_GAP = 12;
constexpr float CARD_WIDTH = 232;
constexpr float SCREEN_SCALE = 1.5f; // the thumbnail, drawn half as large again
constexpr double FLASH_SECONDS = 1.2;
constexpr ImU32 ORANGE = IM_COL32(245, 130, 31, 255);
constexpr ImU32 GREEN_U32 = IM_COL32(97, 187, 70, 255);

ImU32 withAlpha(ImU32 colour, float alpha) {
  const int a = static_cast<int>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}
ImU32 text(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_Text, alpha); }
ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }
ImU32 accent(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_CheckMark, alpha); }

void cardBackground(ImDrawList *draw, ImVec2 a, ImVec2 b, bool hovered) {
  const bool dark = ui::isDark();
  draw->AddRectFilled(a, b, dark ? IM_COL32(255, 255, 255, hovered ? 18 : 10) : IM_COL32(0, 0, 0, hovered ? 14 : 8),
                      CARD_ROUNDING);
  draw->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), CARD_ROUNDING);
}

void dashedRect(ImDrawList *draw, ImVec2 a, ImVec2 b, ImU32 colour) {
  const float dash = 5.0f;
  const float gap = 4.0f;
  auto line = [&](ImVec2 p, ImVec2 q) {
    const float length = std::hypot(q.x - p.x, q.y - p.y);
    const ImVec2 d((q.x - p.x) / length, (q.y - p.y) / length);
    for (float t = 0; t < length; t += dash + gap) {
      const float u = std::min(t + dash, length);
      draw->AddLine(ImVec2(p.x + d.x * t, p.y + d.y * t), ImVec2(p.x + d.x * u, p.y + d.y * u), colour, 1.2f);
    }
  };
  line(a, ImVec2(b.x, a.y));
  line(ImVec2(b.x, a.y), b);
  line(b, ImVec2(a.x, b.y));
  line(ImVec2(a.x, b.y), a);
}

void centredText(ImDrawList *draw, ImVec2 centre, ImU32 colour, const char *line) {
  const ImVec2 size = ImGui::CalcTextSize(line);
  draw->AddText(ImVec2(std::floor(centre.x - size.x * 0.5f), std::floor(centre.y - size.y * 0.5f)), colour, line);
}

// "Just now", "4 minutes ago", "Today, 14:02", "Yesterday, 09:15", or the date.
std::string relativeTime(int64_t milliseconds) {
  const std::time_t then = static_cast<std::time_t>(milliseconds / 1000);
  const std::time_t now = std::time(nullptr);
  const double seconds = std::difftime(now, then);
  if (seconds < 45) return "Just now";
  if (seconds < 3600) {
    const int minutes = std::max(1, static_cast<int>(std::lround(seconds / 60)));
    return std::to_string(minutes) + (minutes == 1 ? " minute ago" : " minutes ago");
  }
  std::tm a{};
  std::tm b{};
  localtime_r(&then, &a);
  localtime_r(&now, &b);
  char clock[16];
  std::strftime(clock, sizeof(clock), "%H:%M", &a);
  if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) return std::string("Today, ") + clock;
  if (a.tm_year == b.tm_year && a.tm_yday + 1 == b.tm_yday) return std::string("Yesterday, ") + clock;
  return formatTime(milliseconds);
}

} // namespace

void SaveStates::flash(const std::string &id, const std::string &message) {
  flashes_[id] = {message, ImGui::GetTime()};
}

void SaveStates::drawFlash(const std::string &id, ImVec2 at, ImVec2 size) {
  auto it = flashes_.find(id);
  if (it == flashes_.end()) return;
  const double age = ImGui::GetTime() - it->second.second;
  if (age > FLASH_SECONDS) {
    flashes_.erase(it);
    return;
  }
  // In quickly, held, then out.
  const float alpha = static_cast<float>(std::min(1.0, std::min(age / 0.12, (FLASH_SECONDS - age) / 0.35)));
  ImDrawList *draw = ImGui::GetWindowDrawList();
  draw->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y), IM_COL32(0, 0, 0, static_cast<int>(110 * alpha)), 6.0f);
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.2f);
  const char *label = it->second.first.c_str();
  const ImVec2 textSize = ImGui::CalcTextSize(label);
  const ImVec2 middle(at.x + size.x * 0.5f, at.y + size.y * 0.5f);
  const float pop = 1.0f + 0.15f * static_cast<float>(std::max(0.0, 1.0 - age / 0.2));
  const ImVec2 half((textSize.x * 0.5f + 34) * pop, (textSize.y * 0.5f + 10) * pop);
  draw->AddRectFilled(ImVec2(middle.x - half.x, middle.y - half.y), ImVec2(middle.x + half.x, middle.y + half.y),
                      withAlpha(GREEN_U32, alpha), half.y);
  // A tick, then the word.
  const ImVec2 tick(middle.x - textSize.x * 0.5f - 18, middle.y);
  const ImVec2 points[3] = {ImVec2(tick.x - 6, tick.y), ImVec2(tick.x - 2, tick.y + 4), ImVec2(tick.x + 6, tick.y - 5)};
  draw->AddPolyline(points, 3, withAlpha(IM_COL32_WHITE, alpha), ImDrawFlags_None, 2.2f);
  draw->AddText(ImVec2(middle.x - textSize.x * 0.5f + 4, middle.y - textSize.y * 0.5f), withAlpha(IM_COL32_WHITE, alpha), label);
  ImGui::PopFont();
}

void SaveStates::drawScreen(const Row &row, ImVec2 at, ImVec2 size) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 end(at.x + size.x, at.y + size.y);
  draw->AddRectFilled(ImVec2(at.x, at.y + 2), ImVec2(end.x, end.y + 2), IM_COL32(0, 0, 0, 50), 7.0f);
  draw->AddRectFilled(at, end, IM_COL32(0x0b, 0x0b, 0x0d, 255), 7.0f);
  if (row.thumbnail != ImTextureID_Invalid) {
    draw->AddImageRounded(ImTextureRef(row.thumbnail), at, end, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 7.0f);
    // The glass: a sheen down from the top, as a tube's face catches light.
    draw->AddRectFilledMultiColor(ImVec2(at.x + 2, at.y + 2), ImVec2(end.x - 2, at.y + size.y * 0.4f),
                                  IM_COL32(255, 255, 255, 22), IM_COL32(255, 255, 255, 22), IM_COL32(255, 255, 255, 0),
                                  IM_COL32(255, 255, 255, 0));
  }
  draw->AddRect(at, end, IM_COL32(255, 255, 255, ui::isDark() ? 28 : 60), 7.0f, 0, 1.0f);
}

// The autosave, across the top: its picture, what it holds, the switch, and
// while it is on, a bar filling toward the next save.
void SaveStates::drawAutosave(float width) {
  const MachineProfile *machine = hooks_.machine();
  if (!machine) return;
  const std::string id = StateStore::autosaveId(machine->key);
  const Row &row = rows_[id];
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const ImVec2 screen(THUMB_WIDTH, THUMB_HEIGHT);
  const float height = screen.y + 24;
  const ImVec2 end(at.x + width, at.y + height);
  cardBackground(draw, at, end, false);

  const ImVec2 pic(at.x + 12, at.y + 12);
  drawScreen(row, pic, screen);
  if (!row.record) centredText(draw, ImVec2(pic.x + screen.x * 0.5f, pic.y + screen.y * 0.5f), IM_COL32(255, 255, 255, 90), "No autosave");
  drawFlash(id, pic, screen);

  const float x = pic.x + screen.x + 16;
  float y = at.y + 14;
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.25f);
  draw->AddText(ImVec2(x, y), text(), "Autosave");
  y += ImGui::GetTextLineHeight() + 2;
  ImGui::PopFont();
  const std::string about = std::string(machine->name) + (row.record ? "  ·  " + relativeTime(row.record->savedAt) : "");
  draw->AddText(ImVec2(x, y), secondary(), about.c_str());
  y += ImGui::GetTextLineHeight() + 10;

  // The switch, and how long until the next save.
  ImGui::SetCursorScreenPos(ImVec2(x, y));
  ui::Switch("Save every 5 seconds", &autosave);
  y += ImGui::GetFrameHeight() + 8;
  const float barWidth = end.x - 16 - x;
  draw->AddRectFilled(ImVec2(x, y), ImVec2(x + barWidth, y + 4), text(0.08f), 2.0f);
  if (autosave && emulation_.powered()) {
    const float t = static_cast<float>(std::clamp((ImGui::GetTime() - lastAutosave_) / AUTOSAVE_SECONDS, 0.0, 1.0));
    draw->AddRectFilled(ImVec2(x, y), ImVec2(x + std::max(4.0f, barWidth * t), y + 4), accent(), 2.0f);
    char next[48];
    std::snprintf(next, sizeof(next), "Next in %.0fs", std::ceil(AUTOSAVE_SECONDS * (1 - t)));
    draw->AddText(ImVec2(x, y + 9), secondary(), next);
  } else {
    draw->AddText(ImVec2(x, y + 9), secondary(), autosave ? "Waits for the machine to be on" : "Off");
  }

  // Load it, and keep a copy of it.
  const float button = ImGui::GetFrameHeight();
  ImGui::SetCursorScreenPos(ImVec2(end.x - 16 - 76 - 8 - 76, at.y + 14));
  ImGui::PushID(id.c_str());
  ImGui::BeginDisabled(!row.record);
  if (ui::Button("Load", ImVec2(76, button), ui::ButtonKind::Primary)) {
    if (auto data = store_.load(id)) {
      if (loadBytes(*data, "the autosave")) flash(id, "Loaded");
    }
  }
  ImGui::SameLine(0, 8);
  if (ui::Button("Export", ImVec2(76, button))) exportRecord(id, machine->key + std::string("-autosave.a2state"));
  ImGui::EndDisabled();
  ImGui::PopID();

  // Where the last session was left, until this one has replaced it.
  const std::string last = StateStore::lastSessionId(machine->key);
  if (const auto it = rows_.find(last); it != rows_.end() && it->second.record) {
    ImGui::SetCursorScreenPos(ImVec2(end.x - 16 - 76 - 8 - 76, at.y + 14 + button + 6));
    ImGui::PushID(last.c_str());
    if (ui::Button("Load Last Session", ImVec2(76 + 8 + 76, button))) {
      if (auto data = store_.load(last)) loadBytes(*data, "the last session");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
      ImGui::SetTooltip("The autosave written when ApplEm last quit, %s",
                        relativeTime(it->second.record->savedAt).c_str());
    }
    ImGui::PopID();
  }

  ImGui::SetCursorScreenPos(at);
  ImGui::Dummy(ImVec2(width, height));
}

// A slot: its picture as a small screen, the slot's number, which machine
// filled it and when. Hovering puts what can be done over the picture; an
// empty slot saves when clicked.
void SaveStates::drawSlot(int slot, ImVec2 at, ImVec2 size) {
  const std::string id = StateStore::slotId(slot);
  const std::string label = "Slot " + std::to_string(slot);
  const Row &row = rows_[id];
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 end(at.x + size.x, at.y + size.y);
  ImGui::PushID(id.c_str());
  ImGui::SetCursorScreenPos(at);
  ImGui::SetNextItemAllowOverlap();
  const bool clicked = ImGui::InvisibleButton("##card", size);
  // From the card's own rectangle: the buttons over the picture would
  // otherwise take the hover from it, hide, give it back, and flicker.
  const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                       ImGui::IsMouseHoveringRect(at, end);
  const ImVec2 screenSize(THUMB_WIDTH * SCREEN_SCALE, THUMB_HEIGHT * SCREEN_SCALE);
  const ImVec2 screen(at.x + (size.x - screenSize.x) * 0.5f, at.y + 10);
  const ImVec2 screenEnd(screen.x + screenSize.x, screen.y + screenSize.y);

  if (!row.record) {
    // Somewhere to save.
    if (hovered) draw->AddRectFilled(at, end, accent(0.08f), CARD_ROUNDING);
    dashedRect(draw, ImVec2(at.x + 1, at.y + 1), ImVec2(end.x - 1, end.y - 1), hovered ? accent() : text(0.22f));
    const ImVec2 middle(screen.x + screenSize.x * 0.5f, screen.y + screenSize.y * 0.5f);
    draw->AddCircleFilled(middle, 22, hovered ? accent() : text(0.08f));
    draw->AddLine(ImVec2(middle.x - 9, middle.y), ImVec2(middle.x + 9, middle.y), hovered ? IM_COL32_WHITE : secondary(), 2.5f);
    draw->AddLine(ImVec2(middle.x, middle.y - 9), ImVec2(middle.x, middle.y + 9), hovered ? IM_COL32_WHITE : secondary(), 2.5f);
    centredText(draw, ImVec2(middle.x, middle.y + 40), hovered ? accent() : secondary(), "Save here");
    if (clicked) {
      if (saveTo(id)) flash(id, "Saved");
    }
  } else {
    cardBackground(draw, at, end, hovered);
    drawScreen(row, screen, screenSize);
  }

  // Its number, as a badge on the picture's corner.
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  const std::string number = std::to_string(slot);
  const ImVec2 badge(screen.x + 8, screen.y + 8);
  draw->AddRectFilled(badge, ImVec2(badge.x + 22, badge.y + 20), row.record ? IM_COL32(0, 0, 0, 160) : text(0.08f), 6.0f);
  centredText(draw, ImVec2(badge.x + 11, badge.y + 10), row.record ? IM_COL32_WHITE : secondary(), number.c_str());
  ImGui::PopFont();

  // What is in it.
  float y = screenEnd.y + 10;
  const float x = screen.x;
  if (row.record) {
    const MachineProfile *current = hooks_.machine();
    const MachineProfile *writer = findMachineProfile(row.record->machine.c_str());
    const bool foreign = writer && current && writer->id != current->id;
    draw->AddText(ImVec2(x, y), text(), label.c_str());
    // The machine, orange when loading it would switch machines.
    const char *machineName = writer ? writer->name : row.record->machine.c_str();
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
    const ImVec2 chipSize = ImGui::CalcTextSize(machineName);
    const ImVec2 chip(screenEnd.x - chipSize.x - 12, y + 1);
    draw->AddRectFilled(chip, ImVec2(screenEnd.x, chip.y + chipSize.y + 4), foreign ? withAlpha(ORANGE, 0.18f) : text(0.07f),
                        (chipSize.y + 4) * 0.5f);
    draw->AddText(ImVec2(chip.x + 6, chip.y + 2), foreign ? ORANGE : secondary(), machineName);
    ImGui::PopFont();
    y += ImGui::GetTextLineHeight() + 2;
    draw->AddText(ImVec2(x, y), secondary(), relativeTime(row.record->savedAt).c_str());
    if (hovered && ImGui::IsMouseHoveringRect(ImVec2(x, y), ImVec2(screenEnd.x, y + ImGui::GetTextLineHeight()))) {
      ImGui::SetTooltip("Saved %s", formatTime(row.record->savedAt).c_str());
    }

    // Over the picture while hovered: Load, then Save, Export and Clear.
    if (hovered || ImGui::IsPopupOpen("##slotmenu")) {
      draw->AddRectFilled(screen, screenEnd, IM_COL32(0, 0, 0, 120), 7.0f);
      const float button = ImGui::GetFrameHeight();
      ImGui::SetCursorScreenPos(ImVec2(screen.x + (screenSize.x - 96) * 0.5f, screen.y + screenSize.y * 0.5f - button - 4));
      if (ui::Button("Load", ImVec2(96, button), ui::ButtonKind::Primary)) {
        if (auto data = store_.load(id)) {
          if (loadBytes(*data, label)) flash(id, "Loaded");
        }
      }
      const float small = (screenSize.x - 24 - 12) / 3;
      ImGui::SetCursorScreenPos(ImVec2(screen.x + 12, screen.y + screenSize.y * 0.5f + 8));
      if (ui::Button("Save", ImVec2(small, button))) {
        if (saveTo(id)) flash(id, "Saved");
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("Save over this slot");
      ImGui::SameLine(0, 6);
      if (ui::Button("Export", ImVec2(small, button))) exportRecord(id, row.record->machine + "-" + id + ".a2state");
      ImGui::SameLine(0, 6);
      if (ui::Button("Clear", ImVec2(small, button))) {
        store_.clear(id);
        stale_ = true;
      }
    }
    // The same, from a right click.
    if (ImGui::BeginPopupContextItem("##slotmenu")) {
      if (ImGui::MenuItem("Load")) {
        if (auto data = store_.load(id)) {
          if (loadBytes(*data, label)) flash(id, "Loaded");
        }
      }
      if (ImGui::MenuItem("Save Over")) {
        if (saveTo(id)) flash(id, "Saved");
      }
      if (ImGui::MenuItem("Export…")) exportRecord(id, row.record->machine + "-" + id + ".a2state");
      ImGui::Separator();
      if (ImGui::MenuItem("Clear")) {
        store_.clear(id);
        stale_ = true;
      }
      ImGui::EndPopup();
    }
  } else {
    draw->AddText(ImVec2(x, y), secondary(), label.c_str());
    y += ImGui::GetTextLineHeight() + 2;
    draw->AddText(ImVec2(x, y), secondary(), "Empty");
  }
  drawFlash(id, screen, screenSize);
  ImGui::PopID();
}

void SaveStates::loadFile(const std::string &path) {
  if (auto data = readFile(path)) {
    loadBytes(*data, baseName(path));
  } else {
    error("Could not read " + path + ".");
  }
}

// The sixth place in the grid: a state from a file.
void SaveStates::drawFileCard(ImVec2 at, ImVec2 size) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 end(at.x + size.x, at.y + size.y);
  ImGui::SetCursorScreenPos(at);
  const bool clicked = ImGui::InvisibleButton("##fromfile", size);
  const bool hovered = ImGui::IsItemHovered();
  if (hovered) draw->AddRectFilled(at, end, accent(0.08f), CARD_ROUNDING);
  dashedRect(draw, ImVec2(at.x + 1, at.y + 1), ImVec2(end.x - 1, end.y - 1), hovered ? accent() : text(0.22f));
  const ImVec2 middle(at.x + size.x * 0.5f, at.y + 10 + THUMB_HEIGHT * SCREEN_SCALE * 0.5f);
  // A document with its corner turned.
  const ImU32 ink = hovered ? accent() : secondary();
  const ImVec2 doc(middle.x - 16, middle.y - 22);
  const ImVec2 points[5] = {ImVec2(doc.x, doc.y), ImVec2(doc.x + 22, doc.y), ImVec2(doc.x + 32, doc.y + 10),
                            ImVec2(doc.x + 32, doc.y + 44), ImVec2(doc.x, doc.y + 44)};
  draw->AddPolyline(points, 5, ink, ImDrawFlags_Closed, 2.0f);
  draw->AddLine(ImVec2(doc.x + 22, doc.y), ImVec2(doc.x + 22, doc.y + 10), ink, 2.0f);
  draw->AddLine(ImVec2(doc.x + 22, doc.y + 10), ImVec2(doc.x + 32, doc.y + 10), ink, 2.0f);
  centredText(draw, ImVec2(middle.x, middle.y + 40), ink, "Load from File…");
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  centredText(draw, ImVec2(middle.x, end.y - 22), secondary(), "An .a2state from either build");
  ImGui::PopFont();
  if (clicked) {
    platform_.openFile("Load a save state", {"a2state"}, [this](const std::string &path) {
      if (path.empty()) return;
      if (auto data = readFile(path)) loadBytes(*data, baseName(path));
      else error("Could not read " + path + ".");
    });
  }
}

void SaveStates::draw(bool *open) {
  if (open && *open) {
    if (!wasOpen_) stale_ = true;
    wasOpen_ = true;
    if (stale_) refresh();

    ui::BeforeWindow("Save States");

    if (ui::BeginWindow("Save States", open, ImGuiWindowFlags_AlwaysAutoResize)) {
      dialogs_.note();
      const float width = CARD_WIDTH * 3 + COLUMN_GAP * 2;
      drawAutosave(width);
      ImGui::Dummy(ImVec2(0, 6));

      // Five slots and a way in from a file, three to a row.
      const ImVec2 grid = ImGui::GetCursorScreenPos();
      const ImVec2 cell(CARD_WIDTH, 10 + THUMB_HEIGHT * SCREEN_SCALE + 10 + ImGui::GetTextLineHeight() * 2 + 14);
      for (int i = 0; i < StateStore::SLOTS + 1; i++) {
        const ImVec2 at(grid.x + (i % 3) * (CARD_WIDTH + COLUMN_GAP), grid.y + (i / 3) * (cell.y + COLUMN_GAP));
        if (i < StateStore::SLOTS) drawSlot(i + 1, at, cell);
        else drawFileCard(at, cell);
      }
      ImGui::SetCursorScreenPos(grid);
      ImGui::Dummy(ImVec2(width, cell.y * 2 + COLUMN_GAP));
      if (ImGui::GetTime() < noticeUntil_) {
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(GREEN_U32), "%s", notice_.c_str());
      }
    }
    ImGui::End();
  } else {
    wasOpen_ = false;
  }

  // A state from another machine: switch to it, then load.
  if (openSwitch_) {
    ImGui::OpenPopup(SWITCH_POPUP);
    openSwitch_ = false;
  }
  dialogs_.placeNext();
  if (ImGui::BeginPopupModal(SWITCH_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("%s was saved on the %s.", pendingWhat_.c_str(), pendingMachineName_.c_str());
    ImGui::TextDisabled("Switching rebuilds the machine; disks and memory now in it are lost.");
    ImGui::Spacing();
    if (ui::Button("Switch and Load", ImVec2(150, 0), ui::ButtonKind::Primary)) {
      ImGui::CloseCurrentPopup();
      const std::vector<uint8_t> data = std::move(pendingLoad_);
      if (hooks_.switchTo(static_cast<MachineId>(pendingMachine_))) {
        importNow(data, pendingWhat_);
        stale_ = true;
      } else {
        error("Could not switch to the " + pendingMachineName_ + ".");
      }
      pendingLoad_.clear();
    }
    ImGui::SameLine();
    if (ui::Button("Cancel", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      pendingLoad_.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (openError_) {
    ImGui::OpenPopup(ERROR_POPUP);
    openError_ = false;
  }
  dialogs_.placeNext();
  if (ImGui::BeginPopupModal(ERROR_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32);
    ImGui::TextUnformatted(error_.c_str());
    ImGui::PopTextWrapPos();
    if (ui::Button("OK", ImVec2(110, 0)) || ImGui::IsKeyPressed(ImGuiKey_Enter) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

} // namespace a2e::native
