/*
 * app.hpp - The native front end's user interface
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <string>

namespace a2e::native {

// Everything drawn each frame: the menu bar, the dock space and the windows.
//
// This is plain C++ over Dear ImGui and knows nothing about Cocoa or Metal;
// main.mm owns those and calls frame() between NewFrame and Render. Anything
// the UI needs from the platform comes through a narrow interface rather
// than an #import, so the UI can be read without the platform in mind.
class App {
public:
  explicit App(std::string settingsDirectory);
  ~App();

  App(const App &) = delete;
  App &operator=(const App &) = delete;

  // Build this frame's UI. Call between ImGui::NewFrame() and ImGui::Render().
  void frame();

  // Whether the user asked to quit from inside the UI.
  bool quitRequested() const { return quitRequested_; }

  // The path ImGui keeps its layout in. It must outlive the ImGui context,
  // which is why the App owns the string.
  const char *iniPath() const { return iniPath_.c_str(); }

private:
  void registerSettingsHandler();
  void drawMenuBar();
  void drawDockSpace();
  void drawScreenWindow();

  std::string settingsDirectory_;
  std::string iniPath_;

  bool showScreen_ = true;
  bool showDemo_ = false;
  bool quitRequested_ = false;
  // The dock space is laid out once, the first time it is seen with nothing
  // docked in it; after that the user's layout comes back from the ini.
  bool layoutChecked_ = false;
};

} // namespace a2e::native
