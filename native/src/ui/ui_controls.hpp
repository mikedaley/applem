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
// An upright slider of the given size, the knob riding a thin track with
// the fill drawn from `fillFrom` (a slider centred on 0dB fills from its
// middle). No label: the caller writes what it is. A double click puts the
// value back at `fillFrom`.
bool VSliderFloat(const char *id, float *value, float min, float max, ImVec2 size, float fillFrom);

// A pop-up button: shows the current choice and opens a menu of them. Use
// as ImGui::BeginCombo/EndCombo.
// A disclosure: a chevron and a title that show or hide what follows. Returns
// whether it is open; the state is kept per window, as ImGui keeps a tree's.
bool Disclosure(const char *label, bool defaultOpen = false);
// The same, open as `*open` says and writing back a click; true on a click.
bool Disclosure(const char *label, bool *open);

// How wide Switch draws with this label, for placing one at a right edge.
float SwitchWidth(const char *label);

// Call after ImGui::NewFrame. Only the focused window answers the mouse: a
// window behind shows no tooltips or hover highlights and does not scroll,
// and a click on it only focuses it, as a Mac's does; the controls under it
// do not hear the click or its release. A click on a title bar still drags
// the window, and its traffic lights work, as AppKit's do on a window behind;
// while a popup is open ImGui deals with clicks itself.
void ClickToFocus();

// Whether the pointer is over a rectangle of the window being drawn, and that
// window is the one under it and answering the mouse. ImGui's own
// IsMouseHoveringRect is geometry alone: it is true through a window in
// front, and in a window behind, which showed a covered window's tooltips.
bool IsHoveringRect(ImVec2 min, ImVec2 max);

// Whether the app's windows may dock into one another and into the main
// window. When not, call BeforeWindow before each window's Begin: it gives
// the window a docking class of its own, which nothing else shares, so it
// stays a window, and takes it out of any dock it is already in.
void SetWindowDocking(bool allowed);
// A window that sizes itself to what it holds, kept no taller than the
// screen it is on: the drives' windows grow by the inspector's height when
// it is shown, and ran off the bottom of the screen. It scrolls instead.
void KeepOnMonitor(const char *name);
void BeforeWindow(const char *name);

// ImGui::Begin, with a macOS window's title bar: the window's own close,
// minimise and zoom buttons on the left, as the main window has them, and the
// title centred in a bar as tall as AppKit's. Close clears *open; minimise
// rolls the window up to its title bar and back; zoom fills the screen the
// window is on and puts it back, and is greyed out on a window that sizes
// itself. The buttons are grey while the window is not the active one, and
// show what they do when the pointer is over them. A docked window keeps
// ImGui's tab and its close button.
bool BeginWindow(const char *name, bool *open, ImGuiWindowFlags flags = 0);

// Where a window's dialogs open: over the window, while it is showing, and
// over the main window when it is not (an action from a menu with the
// window shut). note() inside the window's Begin each frame; placeNext()
// before the dialog's.
class DialogAnchor {
public:
  void note();
  void placeNext() const;

private:
  ImVec2 centre_{0, 0};
  int frame_ = -1;
};

bool BeginPopUpButton(const char *label, const char *preview, float width = 0.0f);
void EndPopUpButton();

// A list of choices in a pop-up button, as ImGui::Combo.
bool PopUpButton(const char *label, int *current, const char *const items[], int count);

} // namespace a2e::native::ui
