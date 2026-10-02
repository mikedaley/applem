/*
 * equalizer_window.hpp - The Equalizer window: tone controls over the output
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "equalizer.hpp"

namespace a2e::native {

class Emulation;

// View > Equalizer: the response curve, a slider a band, the preamp, a
// switch and a few presets. It edits a copy of the settings and hands the
// whole set to the equaliser on every change; the app keeps the copy.
class EqualizerWindow {
public:
  explicit EqualizerWindow(Emulation &emulation) : emulation_(emulation) {}

  // The settings as last applied. The app reads them back after draw() to
  // remember them, and sets them before the first draw.
  Equalizer::Settings settings;

  void draw(bool *open);
  // Whether draw() changed the settings this frame.
  bool changed() const { return changed_; }

private:
  void apply();
  void drawResponse(float width, float height);
  void drawBands();
  void drawFooter(float width);

  Emulation &emulation_;
  bool changed_ = false;
};

} // namespace a2e::native
