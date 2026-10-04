/*
 * ui_theme.hpp - The macOS look: system colours, SF Pro and SF Mono
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "imgui.h"

struct ImFont;

namespace a2e::native::ui {

// Load SF Pro for the interface and SF Mono for numbers. Call once, after
// the ImGui context exists and before the first frame.
void loadFonts();

// The monospaced face, for numbers, addresses, registers and filenames, or
// the default font if SF Mono is not there. Use with ImGui::PushFont.
ImFont *monoFont();

// The smallest text anyone is meant to read, as a fraction of the body text
// (13pt): about 11.5pt, a little over the 11pt macOS keeps as its own floor
// for secondary text. The app is used by people who may be fifty or over, so
// captions, labels, counts and readouts are never smaller than this; a
// caption tells itself from what it labels by its capitals and its colour,
// not by being tiny. Only the parts of a picture that are there to look like
// the thing, a chip's legend on a drawn circuit board, go smaller.
inline constexpr float SMALL_TEXT = 0.88f;

// Colours and shapes from the system: AppKit's named colours resolved under
// the current appearance, and the user's accent colour. Cheap to call every
// frame: it reapplies only when the appearance or the accent has changed,
// so switching to Dark Mode, or picking another accent in System Settings,
// follows at once.
void followSystemAppearance();

// Whether the current appearance is dark.
bool isDark();

// Colours chosen to be read, in either appearance. Each is measured against
// the window's background by WCAG 2's contrast ratio and moved, along its own
// hue, until it reaches the ratio its use needs: 4.5:1 for text, the level
// WCAG asks of body text, and 3:1 for what is deliberately quiet but must
// still be legible. Recomputed whenever the appearance or the accent changes.

// The Apple logo's six stripes, every one readable as text on the window and
// as a fill under textOn(): darkened for a light window, lightened for a
// dark one.
struct Palette {
  ImU32 green, yellow, orange, red, purple, blue;
};
const Palette &palette();

// The accent colour, readable as text: a link, a selected value, a caret.
ImU32 accentText(float alpha = 1.0f);
// Text that is meant to recede (a placeholder, a zero byte) but is still
// legible, at 3:1.
ImU32 faintText();
// Black or white, whichever reads better on the fill.
ImU32 textOn(ImU32 fill);
// WCAG 2's contrast ratio between two colours, the first over the second.
float contrastRatio(ImU32 foreground, ImU32 background);

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
