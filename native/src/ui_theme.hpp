/*
 * ui_theme.hpp - The macOS look: system colours, SF Pro and SF Mono
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

struct ImFont;

namespace a2e::native::ui {

// Load SF Pro for the interface and SF Mono for numbers. Call once, after
// the ImGui context exists and before the first frame.
void loadFonts();

// The monospaced face, for numbers, addresses, registers and filenames, or
// the default font if SF Mono is not there. Use with ImGui::PushFont.
ImFont *monoFont();

// Colours and shapes from the system: AppKit's named colours resolved under
// the current appearance, and the user's accent colour. Cheap to call every
// frame: it reapplies only when the appearance or the accent has changed,
// so switching to Dark Mode, or picking another accent in System Settings,
// follows at once.
void followSystemAppearance();

// Whether the current appearance is dark.
bool isDark();

// The corner radius a macOS window has here, which floating windows match:
// measured on macOS 27, about 12 points.
constexpr float WINDOW_RADIUS = 12.0f;

// Round the windows ImGui makes when a window is dragged out of the main
// one. They are borderless, so the system draws them square; this clips
// their layer to macOS's continuous corner, draws a hairline edge and keeps
// the shadow following the shape. Call once, after the Metal backend is
// initialised.
void roundViewportWindows();

} // namespace a2e::native::ui
