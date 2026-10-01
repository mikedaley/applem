/*
 * test_native_display.cpp - The display settings' rules
 *
 * The same rules the browser's display settings window follows, and pinned
 * for the same reasons: a preset that no longer describes the picture must
 * not keep its name, a saved profile keeps its name and is marked modified
 * instead, Custom never changes anything, and storage loses one bad entry
 * rather than everything.
 */

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "../src/display_settings.hpp"
#include "../src/no_signal_frame.hpp"
#include "machine/machine_profile.hpp"

using namespace a2e;
using namespace a2e::native;

TEST_CASE("The shipped defaults are the Pixel Exact preset", "[display]") {
  DisplaySettings defaults;
  DisplaySettings flat;
  applyValues(flat, findPreset("flat")->values);
  REQUIRE(defaults == flat);
}

TEST_CASE("Each machine has its own screen border", "[display]") {
  REQUIRE(defaultsFor(machineProfile(MachineId::AppleIIe)).overscan == DEFAULT_SCREEN_BORDER);
  REQUIRE(defaultsFor(machineProfile(MachineId::AppleIIc)).overscan == DEFAULT_SCREEN_BORDER);
  // A IIgs draws its own border; a second one round it would be a frame
  // round a frame.
  REQUIRE(defaultsFor(machineProfile(MachineId::AppleIIgs)).overscan == 0);
}

TEST_CASE("Presets set the picture, never the user's calibration", "[display][presets]") {
  for (const MonitorPreset &preset : monitorPresets()) {
    INFO(preset.id);
    for (const char *calibration : {"brightness", "contrast", "saturation", "bezelColor",
                                    "overscan", "screenInset", "sharpness"}) {
      REQUIRE(preset.values.count(calibration) == 0);
    }
    REQUIRE(preset.values.count("colorMode") == 1);
  }
  // Each mono preset has a phosphor and asks for the monochrome decoder.
  REQUIRE(findPreset("green")->values.at("monochromeMode") == 1);
  REQUIRE(findPreset("amber")->values.at("monochromeMode") == 2);
  REQUIRE(findPreset("green")->values.at("colorMode") == COLOR_MONOCHROME);
  REQUIRE(findPreset("composite")->values.at("colorMode") == COLOR_COMPOSITE);
}

TEST_CASE("Editing what a preset owns relabels it Custom", "[display][presets]") {
  std::vector<DisplayProfile> none;
  DisplayState state;
  state.applyPreset("composite", none);
  REQUIRE(state.settings.colorMode == COLOR_COMPOSITE);

  SECTION("a setting the preset does not claim leaves it selected") {
    state.settings.brightness = 80;
    state.markModified("brightness", none);
    REQUIRE(state.preset == "composite");
  }
  SECTION("one it claims drops to Custom and keeps the values") {
    state.settings.scanlines = 77;
    state.markModified("scanlines", none);
    REQUIRE(state.preset == CUSTOM_PRESET);
    REQUIRE(state.settings.scanlines == 77);
  }
  SECTION("Custom changes nothing") {
    state.settings.scanlines = 77;
    state.markModified("scanlines", none);
    const DisplaySettings before = state.settings;
    state.applyPreset(CUSTOM_PRESET, none);
    REQUIRE(state.settings == before);
  }
}

TEST_CASE("A profile keeps everything and is marked modified, not dropped",
          "[display][profiles]") {
  std::vector<DisplayProfile> profiles;
  DisplaySettings mine;
  mine.brightness = 85;
  mine.bezelColor = 0x112233;
  mine.curvature = 40;
  const ProfileUpsert saved = upsertProfile(profiles, "  Mike's telly ", captureValues(mine));
  REQUIRE_FALSE(saved.replaced);
  REQUIRE(saved.profile.name == "Mike's telly");
  REQUIRE(saved.profile.id == "user:mike's telly");

  DisplayState state;
  state.applyPreset(saved.profile.id, profiles);
  // Calibration and bezel come back too, unlike a built-in.
  REQUIRE(state.settings.brightness == 85);
  REQUIRE(state.settings.bezelColor == 0x112233);

  state.settings.brightness = 60;
  state.markModified("brightness", profiles);
  REQUIRE(state.preset == saved.profile.id);
  REQUIRE(state.profileDirty);

  SECTION("saving over the same name replaces it") {
    const ProfileUpsert again = upsertProfile(profiles, "MIKE'S TELLY", captureValues(state.settings));
    REQUIRE(again.replaced);
    REQUIRE(profiles.size() == 1);
    REQUIRE(profiles[0].values.at("brightness") == 60);
  }
  SECTION("a deleted profile becomes Custom without changing the picture") {
    deleteProfile(profiles, saved.profile.id);
    const DisplaySettings before = state.settings;
    state.reconcile(profiles);
    REQUIRE(state.preset == CUSTOM_PRESET);
    REQUIRE(state.settings == before);
  }
}

TEST_CASE("A built-in still selected takes the preset's current values on load",
          "[display][presets]") {
  DisplayState state;
  state.preset = "rgb";
  state.settings.scanlines = 3; // what an older definition said
  state.settings.brightness = 70;
  state.reconcile({});
  REQUIRE(state.settings.scanlines == findPreset("rgb")->values.at("scanlines"));
  REQUIRE(state.settings.brightness == 70);
}

TEST_CASE("Profile names are checked", "[display][profiles]") {
  REQUIRE_FALSE(validateProfileName("   ").ok);
  REQUIRE_FALSE(validateProfileName("Custom").ok);
  REQUIRE_FALSE(validateProfileName(std::string(MAX_PROFILE_NAME_LENGTH + 1, 'x')).ok);
  REQUIRE_FALSE(validateProfileName("two\nlines").ok);
  const NameCheck check = validateProfileName("  Green Glow  ");
  REQUIRE(check.ok);
  REQUIRE(check.name == "Green Glow");
}

TEST_CASE("Profiles survive storage, and a bad one costs only itself",
          "[display][profiles]") {
  std::vector<DisplayProfile> profiles;
  DisplaySettings a;
  a.bezelColor = 0xABCDEF;
  a.maskType = 1;
  upsertProfile(profiles, "First", captureValues(a));
  DisplaySettings b;
  b.monochromeMode = 2;
  upsertProfile(profiles, "Second", captureValues(b));

  const std::string text = serializeProfiles(profiles);
  REQUIRE(text.find("bezelColor=#abcdef") != std::string::npos);
  const std::vector<DisplayProfile> back = parseProfiles(text);
  REQUIRE(back.size() == 2);
  REQUIRE(back[0].values == profiles[0].values);
  REQUIRE(back[1].values == profiles[1].values);

  const std::string damaged = "[Profile]\nName=\ncurvature=1\n\n" + text +
                              "[Profile]\nName=Third\ncurvature=banana\n";
  const std::vector<DisplayProfile> survived = parseProfiles(damaged);
  REQUIRE(survived.size() == 2);
  REQUIRE(survived[0].name == "First");
}

TEST_CASE("Settings reach the shader on the browser's scales", "[display][shader]") {
  DisplaySettings s;
  s.scanlines = 30;
  s.phosphorGlow = 15;
  s.brightness = 100;
  s.bezelColor = 0xFF8000;
  CrtParams p = crtParamsFor(s);
  REQUIRE(p.scanlineIntensity == Approx(0.30f));
  REQUIRE(p.glowIntensity == Approx(0.15f));
  REQUIRE(p.brightness == Approx(1.0f));
  REQUIRE(p.surround[0] == Approx(1.0f));
  REQUIRE(p.surround[1] == Approx(128.0f / 255.0f));
  // A flat screen keeps square corners; a curved or bezelled one rounds them.
  REQUIRE(p.cornerRadius == 0.0f);
  s.curvature = 10;
  REQUIRE(crtParamsFor(s).cornerRadius == Approx(0.02f));
  // The knobs the window does not show take the browser renderer's values.
  REQUIRE(p.edgeHighlight == Approx(0.3f));
  REQUIRE(p.scanlineWidth == Approx(0.25f));
}

TEST_CASE("The no-signal picture names the machine to switch on", "[display]") {
  REQUIRE(noSignalMachineName("Apple IIe Enhanced") == "IIE ENHANCED");
  REQUIRE(noSignalMachineName("Apple IIgs") == "IIGS");
  const std::vector<uint8_t> frame = buildNoSignalFrame(560, 384, "IIE ENHANCED");
  REQUIRE(frame.size() == 560u * 384u * 4u);
  // Opaque black with some lit text on it.
  int lit = 0;
  for (size_t i = 0; i < frame.size(); i += 4) {
    REQUIRE(frame[i + 3] == 0xff);
    lit += frame[i] != 0;
  }
  REQUIRE(lit > 1000);
}
