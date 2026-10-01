/*
 * display.cpp - Display settings: kept per machine, applied, and edited
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "display.hpp"

#include "platform.hpp"

#include "../../src/host/machine_host.hpp"
#include "video/video.hpp"

#include "imgui.h"
#include "imgui_internal.h" // MarkIniSettingsDirty

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

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

bool Display::sliderRow(const char *label, const char *key, const char *tooltip) {
  const SettingField *field = findSettingField(key);
  int &value = current().settings.*(field->member);
  ImGui::SetNextItemWidth(-90.0f);
  const bool edited = ImGui::SliderInt(label, &value, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp);
  if (tooltip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip("%s", tooltip);
  if (edited) changed(key);
  return edited;
}

void Display::drawPresetControls() {
  DisplayState &state = current();

  // The selection's label: a built-in, one of the user's, or Custom.
  std::string label = "Custom";
  if (const DisplayProfile *profile = findProfile(profiles_, state.preset)) {
    label = profile->name + (state.profileDirty ? " (modified)" : "");
  } else if (const MonitorPreset *preset = findPreset(state.preset)) {
    label = preset->label;
  }

  ImGui::SeparatorText("Monitor");
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::BeginCombo("##monitor", label.c_str())) {
    for (const MonitorPreset &preset : monitorPresets()) {
      if (ImGui::Selectable(preset.label, state.preset == preset.id)) {
        state.applyPreset(preset.id, profiles_);
        saved_[machineKey_] = true;
        machineDirty_ = true;
        ImGui::MarkIniSettingsDirty();
      }
    }
    // The user's own get their own group: "Composite Color" is a claim
    // about real hardware, a saved profile is not, and the difference is
    // worth keeping visible.
    if (!profiles_.empty()) {
      ImGui::SeparatorText("My Profiles");
      for (const DisplayProfile &profile : profiles_) {
        ImGui::PushID(profile.id.c_str());
        if (ImGui::Selectable(profile.name.c_str(), state.preset == profile.id)) {
          state.applyPreset(profile.id, profiles_);
          saved_[machineKey_] = true;
          machineDirty_ = true;
          ImGui::MarkIniSettingsDirty();
        }
        ImGui::PopID();
      }
      ImGui::Separator();
    }
    // Custom keeps the current values: it is where editing lands, not
    // somewhere that changes anything.
    if (ImGui::Selectable("Custom", state.preset == CUSTOM_PRESET) &&
        state.preset != CUSTOM_PRESET) {
      state.preset = CUSTOM_PRESET;
      state.profileDirty = false;
      saved_[machineKey_] = true;
      ImGui::MarkIniSettingsDirty();
    }
    ImGui::EndCombo();
  }

  // The description, or a brief confirmation of what was just done.
  const bool flashing = ImGui::GetTime() < statusUntil_;
  ImGui::PushTextWrapPos(0.0f);
  if (flashing) {
    ImGui::TextColored(ImVec4(0.38f, 0.73f, 0.27f, 1.0f), "%s", status_.c_str());
  } else {
    ImGui::TextDisabled("%s", state.description(profiles_).c_str());
  }
  ImGui::PopTextWrapPos();

  // Save writes back to the selected profile, and only means something when
  // there is something to write; Delete only when a profile is selected.
  const DisplayProfile *selected = findProfile(profiles_, state.preset);
  ImGui::BeginDisabled(!selected || !state.profileDirty);
  if (ImGui::Button("Save") && selected) {
    const std::string name = selected->name;
    upsertProfile(profiles_, name, captureValues(state.settings));
    saveProfiles();
    state.profileDirty = false;
    saved_[machineKey_] = true;
    ImGui::MarkIniSettingsDirty();
    flashStatus("Saved to \"" + name + "\".");
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Save As...")) {
    std::snprintf(nameBuffer_, sizeof(nameBuffer_), "%s", selected ? selected->name.c_str() : "");
    nameError_.clear();
    pendingReplace_.clear();
    openSaveAs_ = true;
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!selected);
  if (ImGui::Button("Delete")) openDelete_ = true;
  ImGui::EndDisabled();
}

// The name is the identity: typing an existing name replaces that profile,
// after asking, and a new name branches from the current picture.
void Display::drawSaveAsPopup() {
  if (openSaveAs_) {
    ImGui::OpenPopup(SAVE_AS_POPUP);
    openSaveAs_ = false;
  }
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(SAVE_AS_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

  DisplayState &state = current();
  if (pendingReplace_.empty()) {
    ImGui::TextUnformatted("Save the current display settings as:");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(280.0f);
    const bool submitted = ImGui::InputText("##name", nameBuffer_, sizeof(nameBuffer_),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
    if (!nameError_.empty()) ImGui::TextColored(ImVec4(0.88f, 0.23f, 0.24f, 1.0f), "%s", nameError_.c_str());
    if (ImGui::Button("Save", ImVec2(100, 0)) || submitted) {
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
        flashStatus("Saved as \"" + result.profile.name + "\".");
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
      ImGui::CloseCurrentPopup();
    }
  } else {
    const DisplayProfile *existing = findProfile(profiles_, profileId(pendingReplace_));
    ImGui::Text("A profile called \"%s\" already exists. Replace it?",
                existing ? existing->name.c_str() : pendingReplace_.c_str());
    if (ImGui::Button("Replace", ImVec2(100, 0))) {
      const ProfileUpsert result = upsertProfile(profiles_, pendingReplace_, captureValues(state.settings));
      saveProfiles();
      state.preset = result.profile.id;
      state.profileDirty = false;
      saved_[machineKey_] = true;
      ImGui::MarkIniSettingsDirty();
      flashStatus("Replaced \"" + result.profile.name + "\".");
      pendingReplace_.clear();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
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
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
  if (!ImGui::BeginPopupModal(DELETE_POPUP, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  DisplayState &state = current();
  const DisplayProfile *profile = findProfile(profiles_, state.preset);
  if (!profile) {
    ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return;
  }
  ImGui::Text("Delete the profile \"%s\"? The current picture will not change.", profile->name.c_str());
  if (ImGui::Button("Delete", ImVec2(100, 0))) {
    deleteProfile(profiles_, profile->id);
    saveProfiles();
    state.preset = CUSTOM_PRESET;
    state.profileDirty = false;
    saved_[machineKey_] = true;
    ImGui::MarkIniSettingsDirty();
    ImGui::CloseCurrentPopup();
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel", ImVec2(100, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

bool Display::takeMachineChange() {
  const bool change = machineDirty_;
  machineDirty_ = false;
  return change;
}

void Display::drawWindow(bool *open) {
  ImGui::SetNextWindowSize(ImVec2(360, 560), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Display Settings", open)) {
    DisplayState &state = current();
    ImGui::PushItemWidth(-90.0f);

    drawPresetControls();

    // Calibration, not simulation, and what people reach for most often, so
    // it stays outside the disclosure.
    ImGui::SeparatorText("Image");
    sliderRow("Brightness", "brightness");
    sliderRow("Contrast", "contrast");
    sliderRow("Saturation", "saturation");

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Advanced")) {
      ImGui::SeparatorText("CRT Effects");
      sliderRow("Screen Curvature", "curvature");
      sliderRow("Screen Border", "overscan");
      sliderRow("Scanlines", "scanlines");
      sliderRow("Beam Bloom", "beamBloom",
                "How much a bright line's beam spot widens over a dark one's. Shows through the scanlines.");
      sliderRow("Shadow Mask", "shadowMask");
      sliderRow("Phosphor Glow", "phosphorGlow");
      sliderRow("Vignette", "vignette");
      sliderRow("RGB Offset", "rgbOffset");
      sliderRow("Flicker", "flicker");

      ImGui::SeparatorText("Analog Effects");
      sliderRow("Static Noise", "staticNoise");
      sliderRow("Jitter", "jitter");
      sliderRow("Horizontal Sync", "horizontalSync");
      sliderRow("Glowing Line", "glowingLine");
      sliderRow("Ambient Light", "ambientLight");
      sliderRow("Burn In", "burnIn");

      ImGui::SeparatorText("Bezel");
      sliderRow("Bezel Width", "screenInset");
      float colour[3] = {
          static_cast<float>((state.settings.bezelColor >> 16) & 0xFF) / 255.0f,
          static_cast<float>((state.settings.bezelColor >> 8) & 0xFF) / 255.0f,
          static_cast<float>(state.settings.bezelColor & 0xFF) / 255.0f,
      };
      if (ImGui::ColorEdit3("Bezel Color", colour, ImGuiColorEditFlags_NoInputs)) {
        auto byte = [](float v) { return static_cast<int>(v * 255.0f + 0.5f) & 0xFF; };
        state.settings.bezelColor = (byte(colour[0]) << 16) | (byte(colour[1]) << 8) | byte(colour[2]);
        // No change under a built-in preset, which does not own the bezel,
        // but a saved profile does.
        changed("bezelColor");
      }

      ImGui::SeparatorText("Rendering");
      if (ImGui::Combo("Mask Type", &state.settings.maskType, MASK_TYPES, IM_ARRAYSIZE(MASK_TYPES))) {
        changed("maskType");
      }
      if (ImGui::Combo("Display Mode", &state.settings.monochromeMode, MONOCHROME_MODES,
                       IM_ARRAYSIZE(MONOCHROME_MODES))) {
        changed("monochromeMode");
      }
      bool sharp = state.settings.sharpPixels != 0;
      if (ImGui::Checkbox("Sharp Pixels", &sharp)) {
        state.settings.sharpPixels = sharp ? 1 : 0;
        changed("sharpPixels");
      }
      sliderRow("Edge Sharpness", "sharpness",
                "How hard the seam is between two source dots when the picture is magnified. "
                "0 is plain bilinear; 100 keeps each dot flat and puts the whole transition in "
                "one output pixel. Has no effect with Sharp Pixels on, which is already hard.");

      ImGui::SeparatorText("Phosphor");
      sliderRow("Color Bleed", "colorBleed",
                "Vertical inter-scanline colour blending (CRT phosphor overlap).");
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Reset to Defaults")) {
      // The running machine's own defaults; saved profiles are kept.
      state = DisplayState{};
      state.settings = machine_ ? defaultsFor(*machine_) : DisplaySettings{};
      saved_[machineKey_] = true;
      machineDirty_ = true;
      ImGui::MarkIniSettingsDirty();
    }

    ImGui::PopItemWidth();
    drawSaveAsPopup();
    drawDeletePopup();
  }
  ImGui::End();
}

} // namespace a2e::native
