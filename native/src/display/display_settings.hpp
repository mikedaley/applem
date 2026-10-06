/*
 * display_settings.hpp - What the picture looks like, and the rules for it
 *
 * A port of the browser build's display settings (display-settings-window.js,
 * display-storage.js, display-profiles.js) with no UI in it: the settings
 * themselves, the monitor presets, the user's saved profiles, and how a hand
 * edit relabels the selection. The values are the browser's, on the same
 * 0-100 scales, so a picture tuned in one build is the same in the other.
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "display/crt_params.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace a2e {
struct MachineProfile;
}

namespace a2e::native {

// Which decoder the core runs over the dot stream. In step with
// VideoColorMode in src/core/types.hpp.
enum ColorMode { COLOR_MONOCHROME = 0, COLOR_PIXEL_EXACT, COLOR_RGB_MONITOR, COLOR_COMPOSITE, COLOR_SOLID };

// Every setting is an integer: a percentage for the sliders, an index for the
// choices, 0 or 1 for the toggles and 0xRRGGBB for the bezel colour. That
// lets presets, profiles, storage and "which setting changed" share one
// table of names.
struct DisplaySettings {
  int curvature = 0;
  int overscan = 0; // the Screen Border slider; see defaultsFor()
  int scanlines = 0;
  int beamBloom = 60;
  int shadowMask = 0;
  int maskType = 0; // 0 aperture grille, 1 shadow mask
  int phosphorGlow = 0;
  int vignette = 0;
  int brightness = 100;
  int contrast = 100;
  int saturation = 100;
  int rgbOffset = 0;
  int flicker = 0;
  int staticNoise = 0;
  int jitter = 0;
  int horizontalSync = 0;
  int glowingLine = 0;
  int ambientLight = 0;
  int burnIn = 0;
  int sharpPixels = 1;
  int sharpness = 0;
  int colorBleed = 0;
  int monochromeMode = 0; // 0 colour, 1 green, 2 amber, 3 white
  int colorMode = COLOR_SOLID;
  int screenInset = 0;
  int bezelColor = 0xC8B89A;

  bool operator==(const DisplaySettings &) const = default;
};

// One named setting, for tables of them.
struct SettingField {
  const char *key;
  int DisplaySettings::*member;
};
const std::vector<SettingField> &settingFields();
const SettingField *findSettingField(const std::string &key);

// A partial set of values: what a preset or profile claims to set.
using SettingValues = std::map<std::string, int>;

void applyValues(DisplaySettings &settings, const SettingValues &values);
// Everything, which is what a profile keeps.
SettingValues captureValues(const DisplaySettings &settings);

struct MonitorPreset {
  const char *id;
  const char *label;
  const char *description;
  SettingValues values;
};
const std::vector<MonitorPreset> &monitorPresets();
const MonitorPreset *findPreset(const std::string &id);

// A picture the user saved by name. A profile is identified by its name:
// saving over a name replaces it.
struct DisplayProfile {
  std::string id; // PROFILE_ID_PREFIX + the lower-cased name
  std::string name;
  SettingValues values;
};

constexpr const char *PROFILE_ID_PREFIX = "user:";
constexpr size_t MAX_PROFILE_NAME_LENGTH = 40;
constexpr const char *CUSTOM_PRESET = "custom";

std::string profileId(const std::string &name);
bool isProfileId(const std::string &id);
// The trimmed name, or the reason it will not do.
struct NameCheck {
  bool ok = false;
  std::string name;
  std::string error;
};
NameCheck validateProfileName(const std::string &raw);

// The settings a machine starts with: the shipped defaults, which are the
// Solid Colour preset (the native app's first-run choice; the browser starts
// on Pixel Exact), with that machine's screen border. The 8-bit machines'
// pictures fill the frame and want a strip of glass round them; a IIgs draws
// its own border.
constexpr int DEFAULT_SCREEN_BORDER = 35;
DisplaySettings defaultsFor(const MachineProfile &machine);

// The settings in use for one machine, the selection they came from, and the
// user's saved profiles. All the rules about selection live here.
class DisplayState {
public:
  DisplaySettings settings;
  // A built-in preset id, a profile id, or CUSTOM_PRESET.
  std::string preset = "solid";
  // Whether the selected profile has edits not yet saved into it. Only
  // meaningful while a profile is selected: a built-in drops to Custom the
  // moment it is edited, and so is never "modified".
  bool profileDirty = false;

  // Adopt a preset or profile by id. Custom keeps the current values: it is
  // the label for settings already hand-tuned.
  void applyPreset(const std::string &id, const std::vector<DisplayProfile> &profiles);

  // A setting was changed by hand. A built-in preset that claims that
  // setting drops to Custom; one that does not (brightness, the bezel) stays
  // selected. A profile claims everything, stays selected and is marked
  // modified, so Save has something to write back to.
  void markModified(const std::string &key, const std::vector<DisplayProfile> &profiles);

  // The values the selection claims, or nothing for Custom.
  const SettingValues *selectedValues(const std::vector<DisplayProfile> &profiles) const;
  std::string description(const std::vector<DisplayProfile> &profiles) const;

  // After loading: a profile that no longer exists becomes Custom, and a
  // built-in preset still selected takes its current values, so a change to
  // a preset reaches people who already had it.
  void reconcile(const std::vector<DisplayProfile> &profiles);
};

// The user's profiles, by name.
struct ProfileUpsert {
  DisplayProfile profile;
  bool replaced = false;
};
ProfileUpsert upsertProfile(std::vector<DisplayProfile> &profiles,
                            const std::string &name, const SettingValues &values);
void deleteProfile(std::vector<DisplayProfile> &profiles, const std::string &id);
const DisplayProfile *findProfile(const std::vector<DisplayProfile> &profiles,
                                  const std::string &id);

// Storage. A setting is "key=value", the bezel as #rrggbb. Unknown keys and
// malformed values are skipped rather than failing the whole file.
std::string formatSetting(const SettingField &field, const DisplaySettings &settings);
bool parseSetting(const std::string &line, SettingValues &out);

std::string serializeProfiles(const std::vector<DisplayProfile> &profiles);
std::vector<DisplayProfile> parseProfiles(const std::string &text);

// The shader's parameters for these settings: sliders become 0-1, and the
// knobs the window does not show take the browser renderer's values.
CrtParams crtParamsFor(const DisplaySettings &settings);

} // namespace a2e::native
