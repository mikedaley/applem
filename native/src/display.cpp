/*
 * display.cpp - Display settings: kept per machine, applied, and edited
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "display.hpp"
#include "ui_controls.hpp"
#include "ui_theme.hpp"

#include "platform.hpp"

#include "../../src/host/machine_host.hpp"
#include "video/video.hpp"

#include "imgui.h"
#include "imgui_internal.h" // MarkIniSettingsDirty

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace a2e::native {

namespace {

constexpr const char *SAVE_AS_POPUP = "Save Display Profile";
constexpr const char *DELETE_POPUP = "Delete Display Profile";

constexpr const char *MASK_TYPES[] = {"Aperture Grille", "Shadow Mask"};
constexpr const char *MONOCHROME_MODES[] = {"Color", "Green", "Amber", "White"};

} // namespace

Display::Display(std::string profilesPath) : profilesPath_(std::move(profilesPath)) {
  loadProfiles();
}

DisplayState &Display::current() {
  auto it = machines_.find(machineKey_);
  if (it != machines_.end()) return it->second;
  DisplayState state;
  state.settings = machine_ ? defaultsFor(*machine_) : DisplaySettings{};
  return machines_.emplace(machineKey_, state).first->second;
}

const DisplayState &Display::current() const {
  return const_cast<Display *>(this)->current();
}

void Display::setMachine(const MachineProfile &machine) {
  machine_ = &machine;
  machineKey_ = machine.key;
  current();
  machineDirty_ = true;
}

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------

DisplayState *Display::openSection(const char *name) {
  const std::string key = name;
  const MachineProfile *machine = findMachineProfile(key.c_str());
  if (!machine) return nullptr;
  saved_[key] = true;
  DisplayState &state = machines_[key];
  state = DisplayState{};
  state.settings = defaultsFor(*machine);
  return &state;
}

void Display::readLine(DisplayState *state, const char *line) {
  if (!state) return;
  const std::string text = line;
  if (text.rfind("Preset=", 0) == 0) {
    state->preset = text.substr(7);
    return;
  }
  int dirty = 0;
  if (std::sscanf(line, "ProfileDirty=%d", &dirty) == 1) {
    state->profileDirty = dirty != 0;
    return;
  }
  SettingValues values;
  if (parseSetting(text, values)) applyValues(state->settings, values);
}

void Display::writeAll(ImGuiTextBuffer *out, const char *typeName) const {
  for (const auto &[key, state] : machines_) {
    auto saved = saved_.find(key);
    if (saved == saved_.end() || !saved->second) continue;
    out->appendf("[%s][%s]\n", typeName, key.c_str());
    out->appendf("Preset=%s\n", state.preset.c_str());
    out->appendf("ProfileDirty=%d\n", state.profileDirty ? 1 : 0);
    for (const SettingField &field : settingFields()) {
      out->appendf("%s\n", formatSetting(field, state.settings).c_str());
    }
    out->append("\n");
  }
}

void Display::finishLoading() {
  for (auto &[key, state] : machines_) state.reconcile(profiles_);
  machineDirty_ = true;
}

void Display::loadProfiles() {
  std::ifstream in(profilesPath_);
  if (!in) return;
  std::stringstream text;
  text << in.rdbuf();
  profiles_ = parseProfiles(text.str());
}

// Written to a temporary and renamed into place, so a failure part way
// through never leaves half a file where the user's profiles were.
void Display::saveProfiles() {
  const std::string temporary = profilesPath_ + ".tmp";
  {
    std::ofstream out(temporary, std::ios::trunc);
    if (!out) {
      flashStatus("Could not save the profiles.");
      return;
    }
    out << serializeProfiles(profiles_);
    if (!out) {
      flashStatus("Could not save the profiles.");
      return;
    }
  }
  if (std::rename(temporary.c_str(), profilesPath_.c_str()) != 0) {
    flashStatus("Could not save the profiles.");
  }
}

// ---------------------------------------------------------------------------
// Applying
// ---------------------------------------------------------------------------

void Display::applyToRenderer(ScreenRenderer &renderer) const {
  renderer.setParams(crtParamsFor(settings()));
}

// The decoder first, because switching monochrome off restores whichever
// colour decoder was last chosen; monochrome itself is reached through the
// phosphor choice, so a green-screen preset still knows what to go back to.
void Display::applyToMachine(host::MachineHost &host) const {
  Video *video = host.video();
  if (!video) return;
  const DisplaySettings &s = settings();
  if (s.colorMode != COLOR_MONOCHROME && s.colorMode >= 0 && s.colorMode <= COLOR_SOLID) {
    video->setColorMode(static_cast<VideoColorMode>(s.colorMode));
  }
  video->setMonochrome(s.monochromeMode != 0);
}

// A setting was changed by hand.
void Display::changed(const std::string &key) {
  current().markModified(key, profiles_);
  saved_[machineKey_] = true;
  ImGui::MarkIniSettingsDirty();
  if (key == "colorMode" || key == "monochromeMode") machineDirty_ = true;
}

void Display::flashStatus(const std::string &message) {
  status_ = message;
  statusUntil_ = ImGui::GetTime() + 1.8;
}

namespace {

// The window, in points.
constexpr float WINDOW_WIDTH = 440;
constexpr float TILE_GAP = 8;
constexpr int TILES_PER_ROW = 4;
constexpr float GROUP_ROUNDING = 9;
constexpr float LABEL_WIDTH = 140;
constexpr float ROW_PAD = 4; // above and below each row's control

ImU32 withAlpha(ImU32 colour, float alpha) {
  const int a = static_cast<int>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}
ImU32 rgb(uint32_t c, float alpha = 1.0f) {
  return IM_COL32((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, static_cast<int>(255 * alpha));
}
ImU32 text(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_Text, alpha); }
ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }
ImU32 accent(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_CheckMark, alpha); }

int valueOf(const SettingValues &values, const char *key, int fallback) {
  auto it = values.find(key);
  return it == values.end() ? fallback : it->second;
}

void caption(ImDrawList *draw, ImVec2 at, const char *label) {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.78f);
  draw->AddText(at, secondary(), label);
  ImGui::PopFont();
}

// A small monitor showing a look: the six stripes of Apple's logo in colour
// (soft where the signal is composite), or lines of glowing text on a
// monochrome tube, with scanlines where the look has them and the glass
// over it all.
void drawMonitor(ImDrawList *draw, ImVec2 at, ImVec2 size, const SettingValues &values) {
  const ImVec2 end(at.x + size.x, at.y + size.y);
  draw->AddRectFilled(ImVec2(at.x, at.y + 2), ImVec2(end.x, end.y + 2), IM_COL32(0, 0, 0, 60), 6.0f);
  draw->AddRectFilled(at, end, IM_COL32(0x2a, 0x2a, 0x2d, 255), 6.0f);
  draw->AddRect(at, end, IM_COL32(255, 255, 255, 26), 6.0f);
  const float inset = std::max(4.0f, size.x * 0.06f);
  const ImVec2 screen(at.x + inset, at.y + inset);
  const ImVec2 screenEnd(end.x - inset, end.y - inset);
  const float curve = valueOf(values, "curvature", 0) * 0.08f;
  const float rounding = 2.0f + curve * 0.6f;
  draw->AddRectFilled(screen, screenEnd, IM_COL32(0x06, 0x06, 0x07, 255), rounding);
  draw->PushClipRect(ImVec2(screen.x + 1, screen.y + 1), ImVec2(screenEnd.x - 1, screenEnd.y - 1), true);

  const int colorMode = valueOf(values, "colorMode", COLOR_SOLID);
  const int phosphor = valueOf(values, "monochromeMode", 0);
  const ImVec2 inner(screenEnd.x - screen.x, screenEnd.y - screen.y);
  if (colorMode == COLOR_MONOCHROME || phosphor != 0) {
    static const uint32_t phosphors[] = {0x33ff66, 0x33ff66, 0xffb000, 0xe8e8e8};
    const ImU32 ink = rgb(phosphors[std::clamp(phosphor, 0, 3)]);
    // A prompt and a few lines of text, as blocks of glyphs, with a glow.
    static const int lines[][6] = {{5, 3, 6, 0, 0, 0}, {4, 7, 2, 5, 0, 0}, {6, 4, 0, 0, 0, 0}, {3, 5, 4, 0, 0, 0}};
    const float row = inner.y / 6.0f;
    const float glyph = inner.x / 26.0f;
    for (int l = 0; l < 4; l++) {
      float x = screen.x + glyph * 1.5f;
      const float y = screen.y + row * (l + 0.8f);
      for (int w = 0; w < 6 && lines[l][w]; w++) {
        const ImVec2 a(x, y);
        const ImVec2 b(x + lines[l][w] * glyph, y + row * 0.5f);
        draw->AddRectFilled(ImVec2(a.x - 1.5f, a.y - 1.5f), ImVec2(b.x + 1.5f, b.y + 1.5f), withAlpha(ink, 0.18f), 2.0f);
        draw->AddRectFilled(a, b, withAlpha(ink, 0.85f), 1.0f);
        x = b.x + glyph;
      }
    }
    const float y = screen.y + row * 4.8f;
    draw->AddRectFilled(ImVec2(screen.x + glyph * 1.5f, y), ImVec2(screen.x + glyph * 2.5f, y + row * 0.5f), ink);
    draw->AddRectFilled(ImVec2(screen.x + glyph * 3.0f, y), ImVec2(screen.x + glyph * 4.0f, y + row * 0.5f), ink);
  } else {
    static const uint32_t stripes[] = {0x61bb46, 0xfdb827, 0xf5821f, 0xe03a3e, 0x963d97, 0x009ddc};
    const float band = inner.y / 6.0f;
    const bool soft = colorMode == COLOR_COMPOSITE;
    for (int i = 0; i < 6; i++) {
      const float y0 = screen.y + band * i;
      draw->AddRectFilled(ImVec2(screen.x, y0), ImVec2(screenEnd.x, y0 + band + 0.5f), rgb(stripes[i]));
      // Composite colour bleeds into the next stripe.
      if (soft && i < 5) {
        draw->AddRectFilledMultiColor(ImVec2(screen.x, y0 + band * 0.6f), ImVec2(screenEnd.x, y0 + band * 1.4f),
                                      rgb(stripes[i], 0.0f), rgb(stripes[i], 0.0f), rgb(stripes[i + 1], 0.8f),
                                      rgb(stripes[i + 1], 0.8f));
      }
    }
    // Pixel Exact and Solid Colour are the picture's own pixels: show the grid.
    if (colorMode == COLOR_PIXEL_EXACT || colorMode == COLOR_SOLID) {
      for (float x = screen.x + 6; x < screenEnd.x; x += 6) {
        draw->AddLine(ImVec2(x, screen.y), ImVec2(x, screenEnd.y), IM_COL32(0, 0, 0, 40));
      }
    }
  }
  const int scanlines = valueOf(values, "scanlines", 0);
  if (scanlines > 0) {
    const ImU32 line = IM_COL32(0, 0, 0, static_cast<int>(40 + scanlines * 2.2f));
    for (float y = screen.y + 1; y < screenEnd.y; y += 2.5f) draw->AddLine(ImVec2(screen.x, y), ImVec2(screenEnd.x, y), line);
  }
  const int vignette = valueOf(values, "vignette", 0);
  if (vignette > 0) {
    const ImU32 dark = IM_COL32(0, 0, 0, static_cast<int>(vignette * 3));
    draw->AddRectFilledMultiColor(screen, ImVec2(screen.x + inner.x * 0.25f, screenEnd.y), dark, IM_COL32(0, 0, 0, 0),
                                  IM_COL32(0, 0, 0, 0), dark);
    draw->AddRectFilledMultiColor(ImVec2(screenEnd.x - inner.x * 0.25f, screen.y), screenEnd, IM_COL32(0, 0, 0, 0), dark,
                                  dark, IM_COL32(0, 0, 0, 0));
  }
  draw->PopClipRect();
  // The glass.
  draw->AddRectFilledMultiColor(ImVec2(screen.x + 1, screen.y + 1), ImVec2(screenEnd.x - 1, screen.y + inner.y * 0.45f),
                                IM_COL32(255, 255, 255, 30), IM_COL32(255, 255, 255, 30), IM_COL32(255, 255, 255, 0),
                                IM_COL32(255, 255, 255, 0));
  draw->AddRect(screen, screenEnd, IM_COL32(0, 0, 0, 160), rounding, 0, 1.0f);
}

} // namespace

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

void Display::beginGroup(const char *title, float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  caption(draw, ImVec2(at.x + 4, at.y), title);
  ImGui::Dummy(ImVec2(width, ImGui::GetFontSize() * 0.78f + 6));
  groupStart_ = ImGui::GetCursorScreenPos();
  groupWidth_ = width;
  groupHasRow_ = false;
  // The panel goes behind its rows, but its height is known only after
  // them: draw the rows on top and the panel underneath at the end.
  draw->ChannelsSplit(2);
  draw->ChannelsSetCurrent(1);
}

void Display::rowLabel(const char *label, const char *tooltip) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 top = ImGui::GetCursorScreenPos();
  if (groupHasRow_) {
    draw->AddLine(ImVec2(groupStart_.x + 12, top.y), ImVec2(groupStart_.x + groupWidth_ - 12, top.y), text(0.08f));
  }
  groupHasRow_ = true;
  const float height = ImGui::GetFrameHeight();
  rowTop_ = top.y;
  const ImVec2 labelAt(groupStart_.x + 12, top.y + ROW_PAD + (height - ImGui::GetTextLineHeight()) * 0.5f);
  draw->AddText(labelAt, text(), label);
  if (tooltip && ImGui::IsMouseHoveringRect(labelAt, ImVec2(labelAt.x + LABEL_WIDTH - 20, labelAt.y + height)) &&
      ImGui::IsWindowHovered()) {
    ImGui::SetTooltip("%s", tooltip);
  }
  ImGui::SetCursorScreenPos(ImVec2(groupStart_.x + LABEL_WIDTH, top.y + ROW_PAD));
}

void Display::endRow() {
  ImGui::SetCursorScreenPos(ImVec2(groupStart_.x, rowTop_ + ImGui::GetFrameHeight() + ROW_PAD * 2));
  ImGui::Dummy(ImVec2(groupWidth_, 0));
}

void Display::endGroup() {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float bottom = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y;
  draw->ChannelsSetCurrent(0);
  const ImVec2 end(groupStart_.x + groupWidth_, bottom);
  draw->AddRectFilled(groupStart_, end, ui::isDark() ? IM_COL32(255, 255, 255, 10) : IM_COL32(0, 0, 0, 8), GROUP_ROUNDING);
  draw->AddRect(groupStart_, end, ImGui::GetColorU32(ImGuiCol_Border), GROUP_ROUNDING);
  draw->ChannelsMerge();
  ImGui::Dummy(ImVec2(0, 4));
}

bool Display::sliderRow(const char *label, const char *key, const char *tooltip) {
  const SettingField *field = findSettingField(key);
  int &value = current().settings.*(field->member);
  rowLabel(label, tooltip);
  ImGui::SetNextItemWidth(groupStart_.x + groupWidth_ - 14 - ImGui::GetCursorScreenPos().x);
  const std::string id = std::string("##") + key;
  const bool edited = ui::SliderInt(id.c_str(), &value, 0, 100, "%d%%");
  if (edited) changed(key);
  endRow();
  return edited;
}

bool Display::drawTile(const char *id, const std::string &name, const SettingValues &values, bool selected,
                       ImVec2 at, ImVec2 size) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 end(at.x + size.x, at.y + size.y);
  ImGui::SetCursorScreenPos(at);
  ImGui::PushID(id);
  const bool clicked = ImGui::InvisibleButton("##tile", size);
  const bool hovered = ImGui::IsItemHovered();
  ImGui::PopID();
  const bool dark = ui::isDark();
  draw->AddRectFilled(at, end, selected ? accent(0.12f) : dark ? IM_COL32(255, 255, 255, hovered ? 18 : 8)
                                                               : IM_COL32(0, 0, 0, hovered ? 14 : 6),
                      GROUP_ROUNDING);
  if (selected) {
    draw->AddRect(at, end, accent(), GROUP_ROUNDING, 0, 2.0f);
  } else {
    draw->AddRect(at, end, ImGui::GetColorU32(ImGuiCol_Border), GROUP_ROUNDING);
  }
  const float monitorWidth = size.x - 20;
  const ImVec2 monitor(at.x + 10, at.y + 8);
  drawMonitor(draw, monitor, ImVec2(monitorWidth, monitorWidth * 0.68f), values);
  // The name, and a tick on the one in use.
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.8f);
  const ImVec2 nameSize = ImGui::CalcTextSize(name.c_str());
  const float nameY = monitor.y + monitorWidth * 0.68f + 5;
  draw->PushClipRect(ImVec2(at.x + 6, nameY), ImVec2(end.x - 6, end.y), true);
  draw->AddText(ImVec2(std::max(at.x + 6, at.x + (size.x - nameSize.x) * 0.5f), nameY), selected ? accent() : text(),
                name.c_str());
  draw->PopClipRect();
  ImGui::PopFont();
  if (selected) {
    const ImVec2 badge(end.x - 10, at.y + 10);
    draw->AddCircleFilled(badge, 7.0f, accent());
    const ImVec2 tick[3] = {ImVec2(badge.x - 3, badge.y), ImVec2(badge.x - 1, badge.y + 2.5f), ImVec2(badge.x + 3, badge.y - 2.5f)};
    draw->AddPolyline(tick, 3, IM_COL32_WHITE, ImDrawFlags_None, 1.6f);
  }
  return clicked;
}

void Display::drawGallery(float width) {
  DisplayState &state = current();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float tileWidth = (width - TILE_GAP * (TILES_PER_ROW - 1)) / TILES_PER_ROW;
  const ImVec2 tile(tileWidth, 8 + (tileWidth - 20) * 0.68f + 5 + ImGui::GetFontSize() * 0.8f + 8);
  int index = 0;
  auto place = [&]() {
    const ImVec2 at(start.x + (index % TILES_PER_ROW) * (tileWidth + TILE_GAP),
                    start.y + (index / TILES_PER_ROW) * (tile.y + TILE_GAP));
    index++;
    return at;
  };
  auto choose = [&](const std::string &id) {
    state.applyPreset(id, profiles_);
    saved_[machineKey_] = true;
    machineDirty_ = true;
    ImGui::MarkIniSettingsDirty();
  };
  for (const MonitorPreset &preset : monitorPresets()) {
    if (drawTile(preset.id, preset.label, preset.values, state.preset == preset.id, place(), tile)) choose(preset.id);
  }
  // The user's own after the built-ins: "Composite Color" is a claim about
  // real hardware, a saved profile is not.
  for (const DisplayProfile &profile : profiles_) {
    const std::string name = profile.name + (state.preset == profile.id && state.profileDirty ? " •" : "");
    if (drawTile(profile.id.c_str(), name, profile.values, state.preset == profile.id, place(), tile)) choose(profile.id);
  }
  // Custom is where editing a built-in lands, shown only once it has.
  if (state.preset == CUSTOM_PRESET) {
    drawTile(CUSTOM_PRESET, "Custom", captureValues(state.settings), true, place(), tile);
  }
  const int rows = (index + TILES_PER_ROW - 1) / TILES_PER_ROW;
  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(width, rows * tile.y + (rows - 1) * TILE_GAP));
}

void Display::drawPresetControls() {
  DisplayState &state = current();
  const float width = WINDOW_WIDTH;
  drawGallery(width);

  // What the selection is, or a brief confirmation of what was just done,
  // beside the profile buttons when it fits and above them when not.
  ImGui::Dummy(ImVec2(0, 2));
  const DisplayProfile *selected = findProfile(profiles_, state.preset);
  const bool flashing = ImGui::GetTime() < statusUntil_;
  const std::string line = flashing ? status_ : state.description(profiles_);
  const float buttonsWidth = 64 + 6 + 92 + 6 + 72;
  const ImU32 lineColour = flashing ? IM_COL32(97, 187, 70, 255) : ImGui::GetColorU32(ImGuiCol_TextDisabled);
  ImVec2 at = ImGui::GetCursorScreenPos();
  if (ImGui::CalcTextSize(line.c_str()).x + buttonsWidth + 16 <= width) {
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(at.x, at.y + (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f), lineColour, line.c_str());
  } else {
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + width);
    ImGui::PushStyleColor(ImGuiCol_Text, lineColour);
    ImGui::TextUnformatted(line.c_str());
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();
    at = ImGui::GetCursorScreenPos();
  }

  // Save writes back to the selected profile, and only means something when
  // there is something to write; Delete only when a profile is selected.
  ImGui::SetCursorScreenPos(ImVec2(at.x + width - buttonsWidth, at.y));
  ImGui::BeginDisabled(!selected || !state.profileDirty);
  if (ui::Button("Save", ImVec2(64, 0)) && selected) {
    const std::string name = selected->name;
    upsertProfile(profiles_, name, captureValues(state.settings));
    saveProfiles();
    state.profileDirty = false;
    saved_[machineKey_] = true;
    ImGui::MarkIniSettingsDirty();
    flashStatus("Saved to “" + name + "”.");
  }
  ImGui::EndDisabled();
  ImGui::SameLine(0, 6);
  if (ui::Button("Save As…", ImVec2(92, 0))) {
    std::snprintf(nameBuffer_, sizeof(nameBuffer_), "%s", selected ? selected->name.c_str() : "");
    nameError_.clear();
    pendingReplace_.clear();
    openSaveAs_ = true;
  }
  ImGui::SameLine(0, 6);
  ImGui::BeginDisabled(!selected);
  if (ui::Button("Delete", ImVec2(72, 0))) openDelete_ = true;
  ImGui::EndDisabled();
  ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + ImGui::GetFrameHeight() + 8));
  ImGui::Dummy(ImVec2(width, 0));
}

// The name is the identity: typing an existing name replaces that profile,
// after asking, and a new name branches from the current picture.
void Display::drawSaveAsPopup() {
  if (openSaveAs_) {
    ImGui::OpenPopup(SAVE_AS_POPUP);
    openSaveAs_ = false;
  }
  dialogs_.placeNext();
  if (!ImGui::BeginPopupModal(SAVE_AS_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  DisplayState &state = current();
  if (pendingReplace_.empty()) {
    ImGui::TextUnformatted("Save the current display settings as:");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(280.0f);
    const bool submitted = ImGui::InputText("##name", nameBuffer_, sizeof(nameBuffer_),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
    if (!nameError_.empty()) ImGui::TextColored(ImVec4(0.88f, 0.23f, 0.24f, 1.0f), "%s", nameError_.c_str());
    if (ui::Button("Save", ImVec2(100, 0), ui::ButtonKind::Primary) || submitted) {
      const NameCheck check = validateProfileName(nameBuffer_);
      if (!check.ok) {
        nameError_ = check.error;
      } else if (findProfile(profiles_, profileId(check.name))) {
        pendingReplace_ = check.name;
      } else {
        const ProfileUpsert result = upsertProfile(profiles_, check.name, captureValues(state.settings));
        saveProfiles();
        state.preset = result.profile.id;
        state.profileDirty = false;
        saved_[machineKey_] = true;
        ImGui::MarkIniSettingsDirty();
        flashStatus("Saved as \u201c" + result.profile.name + "\u201d.");
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::SameLine();
    if (ui::Button("Cancel", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ImGui::CloseCurrentPopup();
    }
  } else {
    const DisplayProfile *existing = findProfile(profiles_, profileId(pendingReplace_));
    ImGui::Text("A profile called \"%s\" already exists. Replace it?",
                existing ? existing->name.c_str() : pendingReplace_.c_str());
    if (ui::Button("Replace", ImVec2(100, 0), ui::ButtonKind::Primary)) {
      const ProfileUpsert result = upsertProfile(profiles_, pendingReplace_, captureValues(state.settings));
      saveProfiles();
      state.preset = result.profile.id;
      state.profileDirty = false;
      saved_[machineKey_] = true;
      ImGui::MarkIniSettingsDirty();
      flashStatus("Replaced \u201c" + result.profile.name + "\u201d.");
      pendingReplace_.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ui::Button("Cancel", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      pendingReplace_.clear();
    }
  }
  ImGui::EndPopup();
}

// Deleting forgets a name, not a picture: what is on screen stays, and the
// selection becomes Custom.
void Display::drawDeletePopup() {
  if (openDelete_) {
    ImGui::OpenPopup(DELETE_POPUP);
    openDelete_ = false;
  }
  dialogs_.placeNext();
  if (!ImGui::BeginPopupModal(DELETE_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  DisplayState &state = current();
  const DisplayProfile *profile = findProfile(profiles_, state.preset);
  if (!profile) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  ImGui::Text("Delete the profile \"%s\"? The current picture will not change.", profile->name.c_str());
  if (ui::Button("Delete", ImVec2(100, 0))) {
    deleteProfile(profiles_, profile->id);
    saveProfiles();
    state.preset = CUSTOM_PRESET;
    state.profileDirty = false;
    saved_[machineKey_] = true;
    ImGui::MarkIniSettingsDirty();
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ui::Button("Cancel", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

bool Display::takeMachineChange() {
  const bool change = machineDirty_;
  machineDirty_ = false;
  return change;
}

// A page of settings, in groups.
void Display::drawPage(int page) {
  DisplayState &state = current();
  const float width = WINDOW_WIDTH;
  const float control = width - LABEL_WIDTH - 14;
  switch (page) {
  case 0:
    // Calibration, not simulation: what people reach for most often.
    beginGroup("CALIBRATION", width);
    sliderRow("Brightness", "brightness");
    sliderRow("Contrast", "contrast");
    sliderRow("Saturation", "saturation");
    endGroup();
    beginGroup("PIXELS", width);
    rowLabel("Phosphor");
    ImGui::SetNextItemWidth(control);
    if (ui::PopUpButton("##phosphor", &state.settings.monochromeMode, MONOCHROME_MODES, IM_ARRAYSIZE(MONOCHROME_MODES))) {
      changed("monochromeMode");
    }
    endRow();
    rowLabel("Sharp Pixels");
    {
      bool sharp = state.settings.sharpPixels != 0;
      ImGui::SetCursorScreenPos(ImVec2(groupStart_.x + groupWidth_ - 14 - ui::SwitchWidth("##sharp"), ImGui::GetCursorScreenPos().y));
      if (ui::Switch("##sharp", &sharp)) {
        state.settings.sharpPixels = sharp ? 1 : 0;
        changed("sharpPixels");
      }
    }
    endRow();
    sliderRow("Edge Sharpness", "sharpness",
              "How hard the seam is between two source dots when the picture is magnified. 0 is plain "
              "bilinear; 100 keeps each dot flat. Has no effect with Sharp Pixels on.");
    sliderRow("Color Bleed", "colorBleed", "Colour blending between scanlines, as a tube's phosphors overlap.");
    endGroup();
    break;
  case 1:
    beginGroup("THE TUBE", width);
    sliderRow("Screen Curvature", "curvature");
    sliderRow("Scanlines", "scanlines");
    sliderRow("Beam Bloom", "beamBloom",
              "How much a bright line's beam spot widens over a dark one's. Shows through the scanlines.");
    sliderRow("Phosphor Glow", "phosphorGlow");
    sliderRow("Vignette", "vignette");
    sliderRow("Burn In", "burnIn");
    endGroup();
    beginGroup("THE MASK", width);
    sliderRow("Shadow Mask", "shadowMask");
    rowLabel("Mask Type");
    ImGui::SetNextItemWidth(control);
    if (ui::PopUpButton("##masktype", &state.settings.maskType, MASK_TYPES, IM_ARRAYSIZE(MASK_TYPES))) {
      changed("maskType");
    }
    endRow();
    sliderRow("RGB Offset", "rgbOffset");
    sliderRow("Flicker", "flicker");
    endGroup();
    break;
  case 2:
    beginGroup("INTERFERENCE", width);
    sliderRow("Static Noise", "staticNoise");
    sliderRow("Jitter", "jitter");
    sliderRow("Horizontal Sync", "horizontalSync");
    sliderRow("Glowing Line", "glowingLine");
    sliderRow("Ambient Light", "ambientLight");
    endGroup();
    break;
  default:
    beginGroup("AROUND THE PICTURE", width);
    sliderRow("Screen Border", "overscan");
    sliderRow("Bezel Width", "screenInset");
    rowLabel("Bezel Color");
    {
      float colour[3] = {
          static_cast<float>((state.settings.bezelColor >> 16) & 0xFF) / 255.0f,
          static_cast<float>((state.settings.bezelColor >> 8) & 0xFF) / 255.0f,
          static_cast<float>(state.settings.bezelColor & 0xFF) / 255.0f,
      };
      ImGui::SetCursorScreenPos(ImVec2(groupStart_.x + groupWidth_ - 14 - ImGui::GetFrameHeight() * 1.6f,
                                       ImGui::GetCursorScreenPos().y));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
      if (ImGui::ColorEdit3("##bezel", colour, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
        auto byte = [](float v) { return static_cast<int>(v * 255.0f + 0.5f) & 0xFF; };
        state.settings.bezelColor = (byte(colour[0]) << 16) | (byte(colour[1]) << 8) | byte(colour[2]);
        // No change under a built-in preset, which does not own the bezel,
        // but a saved profile does.
        changed("bezelColor");
      }
      ImGui::PopStyleVar();
    }
    endRow();
    endGroup();
    break;
  }
}

void Display::drawWindow(bool *open) {
  ui::BeforeWindow("Display Settings");
  if (ImGui::Begin("Display Settings", open, ImGuiWindowFlags_AlwaysAutoResize)) {
    dialogs_.note();
    DisplayState &state = current();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    caption(draw, ImGui::GetCursorScreenPos(), "MONITOR");
    ImGui::Dummy(ImVec2(WINDOW_WIDTH, ImGui::GetFontSize() * 0.78f + 4));
    drawPresetControls();

    // The settings, a page at a time.
    const float tabs = 320;
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x + (WINDOW_WIDTH - tabs) * 0.5f, ImGui::GetCursorScreenPos().y));
    ui::SegmentedControl("##page", &page_, {"Picture", "CRT", "Signal", "Frame"}, tabs);
    ImGui::Dummy(ImVec2(0, 4));
    drawPage(page_);

    // Back to the machine's own defaults; saved profiles are kept.
    if (ui::Button("Reset to Defaults")) {
      state = DisplayState{};
      state.settings = machine_ ? defaultsFor(*machine_) : DisplaySettings{};
      saved_[machineKey_] = true;
      machineDirty_ = true;
      ImGui::MarkIniSettingsDirty();
    }
    drawSaveAsPopup();
    drawDeletePopup();
  }
  ImGui::End();
}

} // namespace a2e::native
