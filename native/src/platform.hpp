/*
 * platform.hpp - What the UI needs from the platform, and nothing more
 *
 * App is plain C++ over Dear ImGui. The few things it needs that only Cocoa
 * or Metal can provide come through here, so the UI never imports either.
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "imgui.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace a2e::native {

// A texture the screen is drawn from, refilled with each finished frame.
class ScreenSurface {
public:
  virtual ~ScreenSurface() = default;
  // Replace the picture with an RGBA frame.
  virtual void upload(const uint8_t *rgba, int width, int height) = 0;
  // The texture as ImGui draws it, or ImTextureID_Invalid before the first
  // upload.
  virtual ImTextureID texture() const = 0;
  virtual int width() const = 0;
  virtual int height() const = 0;
};

struct Platform {
  std::unique_ptr<ScreenSurface> screen;
  // Whether Caps Lock is on. It is a toggle, not a held key, so it comes from
  // the system rather than from ImGui's key state.
  std::function<bool()> capsLockOn;
  std::function<void(const std::string &)> setWindowTitle;
};

} // namespace a2e::native
