/*
 * ui_controls.hpp - Controls drawn the way macOS draws them
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "imgui.h"

#include <string>
#include <vector>

namespace a2e::native::ui {

// Dear ImGui's controls are functional and look like nothing in particular.
// These draw the AppKit equivalents in the current appearance and accent:
// capsule push buttons, rounded checkboxes with a tick, radio buttons,
// switches with a sliding knob, sliders with a round knob over a thin
// track, segmented controls, and pop-up buttons with up and down chevrons.
// Each behaves like the ImGui control it replaces (same return values, same
// ids from the label, `##` hiding the rest), so a window swaps one for the
// other without changing its logic.

enum class ButtonKind {
  Normal,
  // The window's default action, filled with the accent colour.
  Primary,
};

bool Button(const char *label, ImVec2 size = ImVec2(0, 0), ButtonKind kind = ButtonKind::Normal);

// A tick box, label to its right.
bool Checkbox(const char *label, bool *value);

// A switch, label to its left as System Settings sets them out.
bool Switch(const char *label, bool *value);

bool RadioButton(const char *label, bool active);
bool RadioButton(const char *label, int *value, int choice);

// One of several, side by side.
bool SegmentedControl(const char *id, int *selected, const std::vector<std::string> &labels,
                      float width = 0.0f);

// A slider with the label to its right, as ImGui's are, and the value
// written beside the track.
bool SliderInt(const char *label, int *value, int min, int max, const char *format = "%d");
bool SliderFloat(const char *label, float *value, float min, float max, const char *format = "%.2f");

// A pop-up button: shows the current choice and opens a menu of them. Use
// as ImGui::BeginCombo/EndCombo.
// A disclosure: a chevron and a title that show or hide what follows. Returns
// whether it is open; the state is kept per window, as ImGui keeps a tree's.
bool Disclosure(const char *label, bool defaultOpen = false);
// The same, open as `*open` says and writing back a click; true on a click.
bool Disclosure(const char *label, bool *open);

// How wide Switch draws with this label, for placing one at a right edge.
float SwitchWidth(const char *label);

// Whether the app's windows may dock into one another and into the main
// window. When not, call BeforeWindow before each window's Begin: it gives
// the window a docking class of its own, which nothing else shares, so it
// stays a window, and takes it out of any dock it is already in.
void SetWindowDocking(bool allowed);
void BeforeWindow(const char *name);

bool BeginPopUpButton(const char *label, const char *preview, float width = 0.0f);
void EndPopUpButton();

// A list of choices in a pop-up button, as ImGui::Combo.
bool PopUpButton(const char *label, int *current, const char *const items[], int count);

} // namespace a2e::native::ui
