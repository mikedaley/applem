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

} // namespace a2e::native::ui
