/*
 * display_settings.cpp - What the picture looks like, and the rules for it
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "display_settings.hpp"

#include "machine/machine_profile.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace a2e::native {

namespace {

std::string trim(const std::string &text) {
  size_t start = 0;
  size_t end = text.size();
  while (start < end && std::isspace(static_cast<unsigned char>(text[start]))) start++;
  while (end > start && std::isspace(static_cast<unsigned char>(text[end - 1]))) end--;
  return text.substr(start, end - start);
}

std::string lower(std::string text) {
  for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// The flat picture Pixel Exact and Solid Colour share: every effect off.
SettingValues flatPicture(int colorMode) {
  return {
      {"curvature", 0},      {"scanlines", 0},   {"beamBloom", 60},
      {"shadowMask", 0},     {"maskType", 0},    {"phosphorGlow", 0},
      {"vignette", 0},       {"rgbOffset", 0},   {"flicker", 0},
      {"staticNoise", 0},    {"jitter", 0},      {"horizontalSync", 0},
      {"glowingLine", 0},    {"ambientLight", 0}, {"burnIn", 0},
      {"colorBleed", 0},     {"monochromeMode", 0}, {"sharpPixels", 1},
      {"colorMode", colorMode},
  };
}

} // namespace

const std::vector<SettingField> &settingFields() {
  static const std::vector<SettingField> fields = {
      {"curvature", &DisplaySettings::curvature},
      {"overscan", &DisplaySettings::overscan},
      {"scanlines", &DisplaySettings::scanlines},
      {"beamBloom", &DisplaySettings::beamBloom},
      {"shadowMask", &DisplaySettings::shadowMask},
      {"maskType", &DisplaySettings::maskType},
      {"phosphorGlow", &DisplaySettings::phosphorGlow},
      {"vignette", &DisplaySettings::vignette},
      {"brightness", &DisplaySettings::brightness},
      {"contrast", &DisplaySettings::contrast},
      {"saturation", &DisplaySettings::saturation},
      {"rgbOffset", &DisplaySettings::rgbOffset},
      {"flicker", &DisplaySettings::flicker},
      {"staticNoise", &DisplaySettings::staticNoise},
      {"jitter", &DisplaySettings::jitter},
      {"horizontalSync", &DisplaySettings::horizontalSync},
      {"glowingLine", &DisplaySettings::glowingLine},
      {"ambientLight", &DisplaySettings::ambientLight},
      {"burnIn", &DisplaySettings::burnIn},
      {"sharpPixels", &DisplaySettings::sharpPixels},
      {"sharpness", &DisplaySettings::sharpness},
      {"colorBleed", &DisplaySettings::colorBleed},
      {"monochromeMode", &DisplaySettings::monochromeMode},
      {"colorMode", &DisplaySettings::colorMode},
      {"screenInset", &DisplaySettings::screenInset},
      {"bezelColor", &DisplaySettings::bezelColor},
  };
  return fields;
}

const SettingField *findSettingField(const std::string &key) {
  for (const SettingField &field : settingFields()) {
    if (key == field.key) return &field;
  }
  return nullptr;
}

void applyValues(DisplaySettings &settings, const SettingValues &values) {
  for (const auto &[key, value] : values) {
    if (const SettingField *field = findSettingField(key)) settings.*(field->member) = value;
  }
}

SettingValues captureValues(const DisplaySettings &settings) {
  SettingValues values;
  for (const SettingField &field : settingFields()) values[field.key] = settings.*(field.member);
  return values;
}

// Each names a real thing a //e was plugged into and sets the whole picture
// in one go. None touches brightness, contrast, saturation, the bezel or the
// screen border: those are the user's calibration and framing, not
// properties of the monitor being imitated.
const std::vector<MonitorPreset> &monitorPresets() {
  static const std::vector<MonitorPreset> presets = {
      {"flat", "Pixel Exact", "No CRT simulation: sharp square pixels.",
       flatPicture(COLOR_PIXEL_EXACT)},
      {"solid", "Solid Colour",
       "Every cell its own colour, no fringing: the picture as drawn, not as a monitor would show it.",
       flatPicture(COLOR_SOLID)},
      {"composite", "Composite Color",
       "Colour TV or composite monitor: soft, with artefact fringing.",
       {
           {"curvature", 0},     {"scanlines", 30},  {"beamBloom", 60},
           {"shadowMask", 30},   {"maskType", 1},    {"phosphorGlow", 15},
           {"vignette", 20},     {"rgbOffset", 6},   {"flicker", 0},
           {"staticNoise", 0},   {"jitter", 0},      {"horizontalSync", 0},
           {"glowingLine", 0},   {"ambientLight", 0}, {"burnIn", 10},
           {"colorBleed", 30},   {"monochromeMode", 0}, {"sharpPixels", 0},
           {"colorMode", COLOR_COMPOSITE},
       }},
      {"rgb", "RGB Monitor", "Separate colour signals: sharp, no composite artefacts.",
       {
           {"curvature", 10},    {"scanlines", 22},  {"beamBloom", 45},
           {"shadowMask", 22},   {"maskType", 0},    {"phosphorGlow", 8},
           {"vignette", 12},     {"rgbOffset", 0},   {"flicker", 0},
           {"staticNoise", 0},   {"jitter", 0},      {"horizontalSync", 0},
           {"glowingLine", 0},   {"ambientLight", 0}, {"burnIn", 5},
           {"colorBleed", 15},   {"monochromeMode", 0}, {"sharpPixels", 1},
           {"colorMode", COLOR_RGB_MONITOR},
       }},
      {"green", "Monochrome Green", "P1 phosphor: long persistence, no mask.",
       {
           {"curvature", 20},    {"scanlines", 32},  {"beamBloom", 70},
           {"shadowMask", 0},    {"maskType", 0},    {"phosphorGlow", 28},
           {"vignette", 25},     {"rgbOffset", 0},   {"flicker", 0},
           {"staticNoise", 0},   {"jitter", 0},      {"horizontalSync", 0},
           {"glowingLine", 0},   {"ambientLight", 0}, {"burnIn", 40},
           {"colorBleed", 25},   {"monochromeMode", 1}, {"sharpPixels", 0},
           {"colorMode", COLOR_MONOCHROME},
       }},
      {"amber", "Monochrome Amber", "P3 phosphor: the warmer of the two mono tubes.",
       {
           {"curvature", 20},    {"scanlines", 32},  {"beamBloom", 70},
           {"shadowMask", 0},    {"maskType", 0},    {"phosphorGlow", 25},
           {"vignette", 25},     {"rgbOffset", 0},   {"flicker", 0},
           {"staticNoise", 0},   {"jitter", 0},      {"horizontalSync", 0},
           {"glowingLine", 0},   {"ambientLight", 0}, {"burnIn", 35},
           {"colorBleed", 25},   {"monochromeMode", 2}, {"sharpPixels", 0},
           {"colorMode", COLOR_MONOCHROME},
       }},
  };
  return presets;
}

const MonitorPreset *findPreset(const std::string &id) {
  for (const MonitorPreset &preset : monitorPresets()) {
    if (id == preset.id) return &preset;
  }
  return nullptr;
}

std::string profileId(const std::string &name) {
  return PROFILE_ID_PREFIX + lower(trim(name));
}

bool isProfileId(const std::string &id) {
  return id.rfind(PROFILE_ID_PREFIX, 0) == 0;
}

NameCheck validateProfileName(const std::string &raw) {
  NameCheck check;
  check.name = trim(raw);
  if (check.name.empty()) {
    check.error = "Give the profile a name.";
  } else if (check.name.size() > MAX_PROFILE_NAME_LENGTH) {
    check.error = "Keep the name under " + std::to_string(MAX_PROFILE_NAME_LENGTH) + " characters.";
  } else if (lower(check.name) == CUSTOM_PRESET) {
    // Custom is the label for settings that match no profile, so a profile
    // by that name could never be told apart from it.
    check.error = "\"Custom\" is reserved. Pick another name.";
  } else if (check.name.find_first_of("\r\n") != std::string::npos) {
    check.error = "Keep the name to one line.";
  } else {
    check.ok = true;
  }
  return check;
}

DisplaySettings defaultsFor(const MachineProfile &machine) {
  DisplaySettings settings;
  settings.overscan = machine.family == MachineFamily::AppleIIgs ? 0 : DEFAULT_SCREEN_BORDER;
  return settings;
}

const SettingValues *DisplayState::selectedValues(
    const std::vector<DisplayProfile> &profiles) const {
  if (const DisplayProfile *profile = findProfile(profiles, preset)) return &profile->values;
  if (const MonitorPreset *builtIn = findPreset(preset)) return &builtIn->values;
  return nullptr;
}

std::string DisplayState::description(const std::vector<DisplayProfile> &profiles) const {
  if (findProfile(profiles, preset)) {
    return profileDirty ? "Your saved profile, with unsaved changes." : "Your saved profile.";
  }
  if (const MonitorPreset *builtIn = findPreset(preset)) return builtIn->description;
  return "Hand-tuned settings.";
}

void DisplayState::applyPreset(const std::string &id,
                               const std::vector<DisplayProfile> &profiles) {
  preset = id;
  profileDirty = false;
  if (const SettingValues *values = selectedValues(profiles)) applyValues(settings, *values);
}

void DisplayState::markModified(const std::string &key,
                                const std::vector<DisplayProfile> &profiles) {
  const SettingValues *values = selectedValues(profiles);
  if (values && values->find(key) == values->end()) return;
  if (isProfileId(preset)) {
    profileDirty = true;
    return;
  }
  preset = CUSTOM_PRESET;
}

void DisplayState::reconcile(const std::vector<DisplayProfile> &profiles) {
  if (isProfileId(preset)) {
    if (!findProfile(profiles, preset)) {
      preset = CUSTOM_PRESET;
      profileDirty = false;
    }
    return;
  }
  profileDirty = false;
  if (const MonitorPreset *builtIn = findPreset(preset)) {
    applyValues(settings, builtIn->values);
  } else {
    preset = CUSTOM_PRESET;
  }
}

ProfileUpsert upsertProfile(std::vector<DisplayProfile> &profiles,
                            const std::string &name, const SettingValues &values) {
  ProfileUpsert result;
  result.profile = {profileId(name), trim(name), values};
  for (DisplayProfile &existing : profiles) {
    if (existing.id == result.profile.id) {
      existing = result.profile;
      result.replaced = true;
      return result;
    }
  }
  profiles.push_back(result.profile);
  return result;
}

void deleteProfile(std::vector<DisplayProfile> &profiles, const std::string &id) {
  profiles.erase(std::remove_if(profiles.begin(), profiles.end(),
                                [&](const DisplayProfile &p) { return p.id == id; }),
                 profiles.end());
}

const DisplayProfile *findProfile(const std::vector<DisplayProfile> &profiles,
                                  const std::string &id) {
  for (const DisplayProfile &profile : profiles) {
    if (profile.id == id) return &profile;
  }
  return nullptr;
}

std::string formatSetting(const SettingField &field, const DisplaySettings &settings) {
  const int value = settings.*(field.member);
  char text[64];
  if (field.member == &DisplaySettings::bezelColor) {
    std::snprintf(text, sizeof(text), "%s=#%06x", field.key, value & 0xFFFFFF);
  } else {
    std::snprintf(text, sizeof(text), "%s=%d", field.key, value);
  }
  return text;
}

bool parseSetting(const std::string &line, SettingValues &out) {
  const size_t equals = line.find('=');
  if (equals == std::string::npos) return false;
  const std::string key = trim(line.substr(0, equals));
  const std::string text = trim(line.substr(equals + 1));
  const SettingField *field = findSettingField(key);
  if (!field || text.empty()) return false;
  try {
    size_t used = 0;
    int value = 0;
    if (text[0] == '#') {
      value = std::stoi(text.substr(1), &used, 16);
      used++;
    } else {
      value = std::stoi(text, &used, 10);
    }
    if (used != text.size()) return false;
    out[key] = value;
    return true;
  } catch (...) {
    return false;
  }
}

// One [Profile] section each: a Name line, then the values.
std::string serializeProfiles(const std::vector<DisplayProfile> &profiles) {
  std::ostringstream out;
  for (const DisplayProfile &profile : profiles) {
    DisplaySettings settings;
    applyValues(settings, profile.values);
    out << "[Profile]\nName=" << profile.name << "\n";
    for (const SettingField &field : settingFields()) {
      if (profile.values.count(field.key)) out << formatSetting(field, settings) << "\n";
    }
    out << "\n";
  }
  return out.str();
}

// A malformed profile costs the user that profile, not the rest.
std::vector<DisplayProfile> parseProfiles(const std::string &text) {
  std::vector<DisplayProfile> profiles;
  std::istringstream in(text);
  std::string line;
  std::optional<DisplayProfile> current;
  auto finish = [&] {
    if (current && !current->name.empty() && !current->values.empty() &&
        !findProfile(profiles, current->id)) {
      profiles.push_back(*current);
    }
    current.reset();
  };
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (trim(line) == "[Profile]") {
      finish();
      current = DisplayProfile{};
    } else if (current && line.rfind("Name=", 0) == 0) {
      current->name = trim(line.substr(5));
      current->id = profileId(current->name);
    } else if (current) {
      parseSetting(line, current->values);
    }
  }
  finish();
  return profiles;
}

CrtParams crtParamsFor(const DisplaySettings &s) {
  auto unit = [](int percent) { return static_cast<float>(percent) / 100.0f; };
  CrtParams p;
  p.curvature = unit(s.curvature);
  p.overscan = unit(s.overscan);
  p.scanlineIntensity = unit(s.scanlines);
  p.beamBloom = unit(s.beamBloom);
  p.shadowMask = unit(s.shadowMask);
  p.maskType = s.maskType;
  p.glowIntensity = unit(s.phosphorGlow);
  p.vignette = unit(s.vignette);
  p.brightness = unit(s.brightness);
  p.contrast = unit(s.contrast);
  p.saturation = unit(s.saturation);
  p.rgbOffset = unit(s.rgbOffset);
  p.flicker = unit(s.flicker);
  p.staticNoise = unit(s.staticNoise);
  p.jitter = unit(s.jitter);
  p.horizontalSync = unit(s.horizontalSync);
  p.glowingLine = unit(s.glowingLine);
  p.ambientLight = unit(s.ambientLight);
  p.burnIn = unit(s.burnIn);
  p.sharpness = unit(s.sharpness);
  p.colorBleed = unit(s.colorBleed);
  p.monochromeMode = s.monochromeMode;
  p.screenInset = unit(s.screenInset);
  p.sharpPixels = s.sharpPixels != 0;
  p.surround[0] = static_cast<float>((s.bezelColor >> 16) & 0xFF) / 255.0f;
  p.surround[1] = static_cast<float>((s.bezelColor >> 8) & 0xFF) / 255.0f;
  p.surround[2] = static_cast<float>(s.bezelColor & 0xFF) / 255.0f;
  // Rounded corners only when there is a curved or bezelled screen to round;
  // a flat picture keeps square ones.
  if (s.screenInset <= 0 && s.curvature <= 0) p.cornerRadius = 0.0f;
  return p;
}

} // namespace a2e::native
