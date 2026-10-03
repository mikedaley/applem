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

#include "crt_params.hpp"
#include "game_port.hpp"
#include "imgui.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace a2e::native {

// The machine's picture, drawn through the CRT chain (crt.metal) into a
// texture ImGui can show.
class ScreenRenderer {
public:
  virtual ~ScreenRenderer() = default;

  // A new frame from the machine, RGBA. Taken at the next render().
  virtual void upload(const uint8_t *rgba, int width, int height) = 0;
  virtual void setParams(const CrtParams &params) = 0;
  // Draw the picture at exactly this many pixels and return the texture it
  // is in, or ImTextureID_Invalid before anything has been uploaded.
  // `pixelRatio` is the display's pixels per point, which the shadow mask's
  // pitch is measured in. Call at most once a frame.
  virtual ImTextureID render(int pixelWidth, int pixelHeight, float pixelRatio) = 0;
  // Forget the phosphor's afterimage, as a tube left off would.
  virtual void clearPersistence() = 0;
  // Why the shader could not be built, or empty.
  virtual std::string error() const = 0;
  // The last picture render() drew, as RGBA rows from the top. Waits for the
  // GPU, so it is for screenshots and tests, not for every frame.
  virtual bool readPixels(std::vector<uint8_t> &rgba, int &width, int &height) = 0;
};

struct Platform {
  std::unique_ptr<ScreenRenderer> screen;
  // Whether Caps Lock is on. It is a toggle, not a held key, so it comes from
  // the system rather than from ImGui's key state.
  std::function<bool()> capsLockOn;
  std::function<void(const std::string &)> setWindowTitle;
  // The app's appearance: 0 follows the system, 1 is light, 2 is dark.
  std::function<void(int)> setAppearance;
  // Enter or leave macOS full screen for the main window.
  std::function<void()> toggleFullScreen;
  // Resize the main window so its content is this size in points, kept on
  // its screen. Nothing happens in full screen, where the system owns it.
  std::function<void(float width, float height)> setMainContentSize;
  // The largest content the main window can have on the screen it is on.
  std::function<ImVec2()> mainContentLimit;

  // The open and save panels. Both return at once; `done` runs later on the
  // main thread, between frames, with the chosen path or an empty string if
  // the user cancelled. Extensions are without the dot.
  using FileChosen = std::function<void(const std::string &path)>;
  std::function<void(const std::string &title, const std::vector<std::string> &extensions,
                     FileChosen done)>
      openFile;
  std::function<void(const std::string &title, const std::string &suggestedName,
                     const std::vector<std::string> &extensions, FileChosen done)>
      saveFile;

  // The mouse, taken for the machine: the pointer hidden and held where it
  // is, and its movement and left button handed over instead of reaching the
  // windows. Control-Option, pressed and let go with no other key between,
  // gives it back, as it does in UTM and Parallels, and so does leaving the
  // app; either way `released` says so on the next take.
  struct MouseInput {
    float dx = 0, dy = 0;       // points moved, down positive, since the last take
    std::vector<bool> buttons;  // the left button's changes, oldest first
    bool released = false;
  };
  std::function<void(bool captured)> captureMouse;
  std::function<MouseInput()> takeMouseInput;

  // Open a web page in the user's browser.
  std::function<void(const std::string &url)> openURL;

  // The bundle's Resources, where the disk library is.
  std::string resourceDirectory;

  // A small RGBA texture ImGui can draw (a save state's thumbnail), and
  // letting one go.
  std::function<ImTextureID(const uint8_t *rgba, int width, int height)> makeTexture;
  std::function<void(ImTextureID)> releaseTexture;

  // The connected gamepads, in the browser's standard layout.
  std::function<std::vector<Pad>()> gamepads;
};

} // namespace a2e::native
