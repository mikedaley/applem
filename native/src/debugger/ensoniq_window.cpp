/*
 * ensoniq_window.cpp - The Ensoniq window: a IIgs's 5503 DOC and its sound RAM
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "debugger/ensoniq_window.hpp"

#include "app/emulation.hpp"
#include "ui/ui_controls.hpp"
#include "ui/ui_theme.hpp"

#include "iigs/iigs_machine.hpp"
#include "iigs/iigs_sound.hpp"

#include "imgui.h"
#include "imgui_internal.h" // SetItemKeyOwner

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

namespace a2e::native {

namespace {

using iigs::IIgsSound;

constexpr int DOC_OSCILLATORS = iigs::DOC_OSCILLATOR_COUNT;

constexpr float WIDTH = 1000;
constexpr float CARD_ROUNDING = 10;
constexpr float PAD = 14;
constexpr float ROW_HEIGHT = 42;
constexpr float RAM_HEIGHT = 72;
// The fewest bytes the sound RAM view zooms in to.
constexpr double RAM_MIN_SPAN = 32;

constexpr const char *NOTE_NAMES[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
constexpr const char *MODE_NAMES[4] = {"FREE", "ONE", "SYNC", "SWAP"};
constexpr const char *MODE_TIPS[4] = {
    "Free run: loops its table until stopped",
    "One shot: plays its table once and halts",
    ("Sync: an even oscillator restarts the odd one below it at its end; an odd one modulates the even one "
     "above it (AM)"),
    "Swap: halts at the end of its table and starts its partner",
};

ImU32 text(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_Text, alpha); }
ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }

ImU32 withAlpha(ImU32 colour, float alpha) {
  const int a = static_cast<int>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

void card(ImDrawList *draw, ImVec2 a, ImVec2 b) {
  draw->AddRectFilled(a, b, ui::isDark() ? IM_COL32(255, 255, 255, 10) : IM_COL32(0, 0, 0, 8), CARD_ROUNDING);
  draw->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), CARD_ROUNDING);
}

ImU32 well() { return ui::isDark() ? IM_COL32(0, 0, 0, 90) : IM_COL32(0, 0, 0, 18); }

void caption(ImDrawList *draw, ImVec2 at, const char *label) {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  draw->AddText(at, secondary(), label);
  ImGui::PopFont();
}

// A capsule with a word in it, lit or not, as wide as `widest` would make
// it so a capsule whose word changes keeps its size, and everything after
// it its place. Returns its width.
float pill(ImDrawList *draw, ImVec2 at, const char *label, bool lit, ImU32 colour, const char *widest = nullptr) {
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  const ImVec2 size = ImGui::CalcTextSize(label);
  const float inner = widest ? std::max(size.x, ImGui::CalcTextSize(widest).x) : size.x;
  const ImVec2 end(at.x + inner + 10, at.y + size.y + 4);
  draw->AddRectFilled(at, end, lit ? colour : text(0.07f), (end.y - at.y) * 0.5f);
  draw->AddText(ImVec2(at.x + 5 + (inner - size.x) * 0.5f, at.y + 2), lit ? ui::textOn(colour) : secondary(), label);
  ImGui::PopFont();
  return end.x - at.x;
}

float pillHeight() {
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  const float h = ImGui::GetTextLineHeight() + 4;
  ImGui::PopFont();
  return h;
}

// A label and a value in the mono face, the value coloured. The width is the
// label and the widest value it can hold, whatever it holds now, so the
// fields after it never move. Returns that width.
float field(ImDrawList *draw, ImVec2 at, const char *label, const char *value, ImU32 colour, const char *widest) {
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  draw->AddText(at, secondary(), label);
  const float lw = ImGui::CalcTextSize(label).x + 5;
  draw->AddText(ImVec2(at.x + lw, at.y), colour, value);
  const float width = lw + std::max(ImGui::CalcTextSize(widest).x, ImGui::CalcTextSize(value).x);
  ImGui::PopFont();
  return width;
}

// A speaker, with sound coming out of it or a cross where the sound was.
void speaker(ImDrawList *draw, ImVec2 c, bool muted, ImU32 colour) {
  draw->AddRectFilled(ImVec2(c.x - 6, c.y - 2.5f), ImVec2(c.x - 3, c.y + 2.5f), colour);
  draw->AddTriangleFilled(ImVec2(c.x - 3, c.y - 2.5f), ImVec2(c.x + 1.5f, c.y - 6.5f), ImVec2(c.x + 1.5f, c.y + 6.5f), colour);
  draw->AddTriangleFilled(ImVec2(c.x - 3, c.y - 2.5f), ImVec2(c.x + 1.5f, c.y + 6.5f), ImVec2(c.x - 3, c.y + 2.5f), colour);
  if (muted) {
    draw->AddLine(ImVec2(c.x + 3.5f, c.y - 3), ImVec2(c.x + 8.5f, c.y + 3), colour, 1.6f);
    draw->AddLine(ImVec2(c.x + 8.5f, c.y - 3), ImVec2(c.x + 3.5f, c.y + 3), colour, 1.6f);
  } else {
    draw->PathArcTo(ImVec2(c.x + 1.5f, c.y), 5.0f, -0.9f, 0.9f, 8);
    draw->PathStroke(colour, 0, 1.4f);
  }
}

// An oscillator's colour: the logo's stripes in turn, a pair sharing one so
// the partners read as partners.
ImU32 hueFor(int index) {
  const ui::Palette &p = ui::palette();
  const ImU32 hues[6] = {p.blue, p.green, p.orange, p.purple, p.yellow, p.red};
  return hues[(index / 2) % 6];
}

const char *sizeName(uint32_t length) {
  switch (length) {
  case 256: return "256";
  case 512: return "512";
  case 1024: return "1K";
  case 2048: return "2K";
  case 4096: return "4K";
  case 8192: return "8K";
  case 16384: return "16K";
  case 32768: return "32K";
  }
  return "?";
}

using WaveView = EnsoniqWindow::WaveView;

// A view kept inside what it shows: never wider than all of it, never
// narrower than `minSpan` bytes, and never past either end.
void clampView(WaveView &v, double total, double minSpan) {
  if (v.span <= 0) v.span = total;
  v.span = std::clamp(v.span, std::min(minSpan, total), total);
  v.start = std::clamp(v.start, 0.0, total - v.span);
}

void zoomAbout(WaveView &v, double at, double factor, double total, double minSpan) {
  const double span = std::clamp(v.span * factor, std::min(minSpan, total), total);
  v.start = std::clamp(at - (at - v.start) * (span / v.span), 0.0, total - span);
  v.span = span;
}

// The pointer over a wave: scroll to zoom about it, drag to pan,
// double-click to see all of it. Returns whether it is over the wave.
bool interactWave(const char *id, ImVec2 a, ImVec2 b, WaveView &v, double total, double minSpan) {
  ImGuiIO &io = ImGui::GetIO();
  ImGui::SetCursorScreenPos(a);
  ImGui::InvisibleButton(id, ImVec2(b.x - a.x, b.y - a.y));
  const bool hovered = ImGui::IsItemHovered();
  const float width = b.x - a.x;
  if (hovered) {
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    if (io.MouseWheel != 0) {
      zoomAbout(v, v.start + (io.MousePos.x - a.x) / width * v.span, std::pow(0.8, io.MouseWheel), total, minSpan);
    }
    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      v.start = 0;
      v.span = total;
    }
    if (v.span < total) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  }
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
    v.start = std::clamp(v.start - io.MouseDelta.x / width * v.span, 0.0, total - v.span);
  }
  return hovered;
}

// Zoom out, zoom in and Fit, ending at `right`, with what is in view written
// to their left. Returns the width they took.
void zoomControls(const char *id, ImDrawList *draw, float right, float y, float height, WaveView &v, double total,
                  double minSpan, const char *range) {
  ImGui::PushID(id);
  const float fitWidth = ImGui::CalcTextSize("Fit").x + ImGui::GetStyle().FramePadding.x * 2;
  const float button = ImGui::GetFrameHeight() * 1.3f;
  const float bx = right - fitWidth - 2 * (button + 4);
  const float by = y + (height - ImGui::GetFrameHeight()) * 0.5f;
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  const float rw = ImGui::CalcTextSize(range).x;
  draw->AddText(ImVec2(bx - 12 - rw, y + (height - ImGui::GetTextLineHeight()) * 0.5f), secondary(), range);
  ImGui::PopFont();
  const double middle = v.start + v.span * 0.5;
  ImGui::SetCursorScreenPos(ImVec2(bx, by));
  ImGui::BeginDisabled(v.span >= total);
  if (ui::Button("\xE2\x88\x92##out", ImVec2(button, 0))) zoomAbout(v, middle, 2.0, total, minSpan);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Zoom out");
  ImGui::SameLine(0, 4);
  ImGui::BeginDisabled(v.span <= std::min(minSpan, total));
  if (ui::Button("+##in", ImVec2(button, 0))) zoomAbout(v, middle, 0.5, total, minSpan);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("Zoom in, or scroll over the wave; drag to move along it");
  }
  ImGui::SameLine(0, 4);
  ImGui::BeginDisabled(v.span >= total);
  if (ui::Button("Fit")) {
    v.start = 0;
    v.span = total;
  }
  ImGui::EndDisabled();
  ImGui::PopID();
}

// The bytes in view, from `base` on (wrapping round the 64K), drawn across
// [a, b): further out a column a pixel, the lowest and highest sample under
// it; close in every byte its own step, joined to the next, with its value
// written on it when there is room. A zero byte stops an oscillator, so it
// is marked rather than drawn as a sample.
void drawWave(ImDrawList *draw, ImVec2 a, ImVec2 b, const std::vector<uint8_t> &ram, uint32_t base,
              const WaveView &v, ImU32 colour) {
  if (ram.size() != iigs::SOUND_RAM_SIZE) return;
  const ui::Palette &p = ui::palette();
  const float width = b.x - a.x;
  const float mid = (a.y + b.y) * 0.5f;
  const float half = (b.y - a.y - 8) * 0.5f;
  const double perPixel = v.span / width;
  auto at = [&](int64_t offset) { return static_cast<int>(ram[(base + static_cast<uint32_t>(offset)) & 0xFFFF]); };
  auto yFor = [&](int value) { return mid - half * ((value - 0x80) / 128.0f); };
  draw->AddLine(ImVec2(a.x, mid), ImVec2(b.x, mid), text(0.1f));

  if (perPixel >= 1.0) {
    const int columns = static_cast<int>(width);
    for (int c = 0; c < columns; c++) {
      const int64_t o0 = static_cast<int64_t>(v.start + c * perPixel);
      const int64_t o1 = std::max<int64_t>(static_cast<int64_t>(v.start + (c + 1) * perPixel), o0 + 1);
      int lo = 0x80, hi = 0x80;
      bool any = false, zero = false;
      for (int64_t o = o0; o < o1; o++) {
        const int value = at(o);
        if (!value) {
          zero = true;
          continue;
        }
        lo = any ? std::min(lo, value) : value;
        hi = any ? std::max(hi, value) : value;
        any = true;
      }
      const float x = a.x + c;
      // A zero on its own in a column is the end of a table; a column of
      // nothing but zeros is just empty memory, and left so.
      if (zero && any && perPixel < 64) {
        draw->AddLine(ImVec2(x + 0.5f, a.y + 3), ImVec2(x + 0.5f, b.y - 3), withAlpha(p.red, 0.6f));
      }
      if (!any) continue;
      draw->AddRectFilled(ImVec2(x, yFor(hi) - 0.5f), ImVec2(x + 1, yFor(lo) + 0.5f), colour);
    }
    return;
  }

  const int64_t first = static_cast<int64_t>(v.start);
  const int64_t last = static_cast<int64_t>(std::ceil(v.start + v.span));
  const float step = static_cast<float>(1.0 / perPixel);
  auto xFor = [&](int64_t offset) { return a.x + static_cast<float>((offset - v.start) / perPixel); };
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  const bool values = step >= ImGui::CalcTextSize("FF").x + 6;
  const float lineHeight = ImGui::GetTextLineHeight();
  for (int64_t o = first; o < last; o++) {
    const int value = at(o);
    const float x0 = xFor(o), x1 = xFor(o + 1);
    if (!value) {
      draw->AddRectFilled(ImVec2(x0, a.y + 2), ImVec2(x1 - 1, b.y - 2), withAlpha(p.red, 0.18f));
      continue;
    }
    const float vy = yFor(value);
    draw->AddLine(ImVec2(x0, vy), ImVec2(x1, vy), colour, 2.0f);
    if (o + 1 < last && at(o + 1)) draw->AddLine(ImVec2(x1, vy), ImVec2(x1, yFor(at(o + 1))), withAlpha(colour, 0.5f));
    if (values) {
      char hex[4];
      std::snprintf(hex, sizeof hex, "%02X", value);
      const float tw = ImGui::CalcTextSize(hex).x;
      const float ty = value >= 0x80 ? vy + 3 : vy - lineHeight - 2;
      draw->AddText(ImVec2((x0 + x1 - tw) * 0.5f, std::clamp(ty, a.y + 1, b.y - lineHeight - 1)), secondary(), hex);
    }
  }
  ImGui::PopFont();
}

// Addresses along the bottom of a wave, at a power of two that gives about
// eight across.
void drawTicks(ImDrawList *draw, float left, float right, float y, uint32_t base, const WaveView &v) {
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  uint32_t tick = 0x2000;
  while (tick > 1 && v.span / tick < 6) tick /= 2;
  const double perPixel = v.span / (right - left);
  const float labelWidth = ImGui::CalcTextSize("$0000").x;
  for (uint64_t t = static_cast<uint64_t>(std::ceil(v.start / tick)) * tick; t < v.start + v.span; t += tick) {
    char label[8];
    std::snprintf(label, sizeof label, "$%04X", static_cast<unsigned>((base + t) & 0xFFFF));
    const float lx = left + static_cast<float>((t - v.start) / perPixel);
    draw->AddLine(ImVec2(lx, y), ImVec2(lx, y + 4), text(0.2f));
    if (lx + 3 + labelWidth <= right) draw->AddText(ImVec2(lx + 3, y + 1), secondary(), label);
  }
  ImGui::PopFont();
}

} // namespace

EnsoniqWindow::EnsoniqWindow(Emulation &emulation) : emulation_(emulation) {}

void EnsoniqWindow::update() {
  emulation_.poll(updatePoll_, [&](host::MachineHost &host) {
    iigs::IIgsMachine *gs = host.iigs();
    present_ = gs != nullptr;
    if (!gs) return;
    // Asked of the chip rather than remembered by its address, which a
    // machine rebuilt with a different amount of memory often reuses.
    IIgsSound &sound = gs->memory().sound();
    for (int i = 0; i < DOC_OSCILLATORS; i++) {
      const bool muted = mutes & (1u << i);
      if (sound.oscillatorMuted(i) != muted) sound.setOscillatorMuted(i, muted);
    }
  });
}

void EnsoniqWindow::setMute(int index, bool muted) {
  const uint32_t bit = 1u << index;
  mutes = muted ? (mutes | bit) : (mutes & ~bit);
  emulation_.post([index, muted](host::MachineHost &host) {
    if (iigs::IIgsMachine *gs = host.iigs()) gs->memory().sound().setOscillatorMuted(index, muted);
  });
}

void EnsoniqWindow::take() {
  emulation_.poll(takePoll_, [&](host::MachineHost &host) {
    iigs::IIgsMachine *gs = host.iigs();
    if (!gs) return;
    const IIgsSound &sound = gs->memory().sound();
    enabled_ = sound.activeOscillators();
    sampleRate_ = sound.sampleRate();
    irq_ = sound.interruptPending();
    interruptRegister_ = sound.docRegister(IIgsSound::DOC_INTERRUPT);
    control_ = sound.readControl();
    address_ = sound.address();
    for (int i = 0; i < DOC_OSCILLATORS; i++) {
      const IIgsSound::Oscillator o = sound.oscillator(i);
      Oscillator &osc = oscillators_[i];
      osc.frequency = o.frequency;
      osc.volume = o.volume;
      osc.control = o.control;
      osc.tableSize = o.tableSize;
      osc.data = o.data;
      osc.interruptPending = o.interruptPending;
      osc.pointer = o.waveTablePointer;
      osc.accumulator = o.accumulator;
      // The table: its size and resolution from $C0, and its start from the
      // pointer, the bank bit and as many low bits masked as the table is
      // long, as the chip finds it.
      const int sizeCode = (o.tableSize & IIgsSound::SIZE_TABLE_MASK) >> 3;
      osc.resolution = o.tableSize & IIgsSound::SIZE_RESOLUTION_MASK;
      osc.length = 256u << sizeCode;
      const uint32_t pointer = (static_cast<uint32_t>(o.waveTablePointer) << 8) |
                               ((o.tableSize & IIgsSound::SIZE_BANK) ? 0x10000u : 0u);
      osc.start = pointer & ~(osc.length - 1) & 0xFFFF;
      const int shift = 9 + osc.resolution - sizeCode;
      osc.position = (o.accumulator >> shift) & (osc.length - 1);
    }
    ram_.resize(iigs::SOUND_RAM_SIZE);
    for (size_t a = 0; a < ram_.size(); a++) ram_[a] = sound.soundRam(static_cast<uint16_t>(a));
  });
}

// The chip, and the window the 65816 reaches it through.
void EnsoniqWindow::drawChip(float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ui::Palette &p = ui::palette();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float line = ImGui::GetTextLineHeight();
  const float height = PAD + line + 10 + line + PAD;
  card(draw, start, ImVec2(start.x + width, start.y + height));

  const float left = start.x + PAD;
  float y = start.y + PAD;
  draw->AddText(ImVec2(left, y), text(), "Ensoniq 5503");
  const float tw = ImGui::CalcTextSize("Ensoniq 5503").x;
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  draw->AddText(ImVec2(left + tw + 10, y + 2), secondary(), "DOC  \xC2\xB7  64K sound RAM  \xC2\xB7  $C03C-$C03F");
  ImGui::PopFont();

  // On the right: whether the oscillators are listed at all, and whether
  // every one is shown or only those the chip is scanning, which the sound
  // RAM's lanes follow too.
  const float switchWidth = ui::SwitchWidth("Show all 32");
  const float listWidth = ui::SwitchWidth("Oscillators");
  ImGui::SetCursorScreenPos(ImVec2(start.x + width - PAD - switchWidth - 24 - listWidth, y - 3));
  ui::Switch("Oscillators", &showOscillators);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("List the oscillators below the sound RAM");
  ImGui::SetCursorScreenPos(ImVec2(start.x + width - PAD - switchWidth, y - 3));
  ui::Switch("Show all 32", &showAll_);
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Every oscillator, not only the %d the chip is scanning", enabled_);
  }
  y += line + 10;

  char value[48];
  float x = left;
  std::snprintf(value, sizeof value, "%d", enabled_);
  x += field(draw, ImVec2(x, y), "OSCILLATORS", value, ui::accentText(), "32") + 18;
  std::snprintf(value, sizeof value, "%.0f Hz", sampleRate_);
  // Fastest with one oscillator: the clock over 8 over 3, six figures.
  x += field(draw, ImVec2(x, y), "RATE", value, text(), "000000 Hz") + 18;
  std::snprintf(value, sizeof value, "$%02X", interruptRegister_);
  x += field(draw, ImVec2(x, y), "$E0", value, p.purple, "$FF") + 10;
  const float py = y - 1;
  x += pill(draw, ImVec2(x, py), "IRQ", irq_, p.red) + 26;

  // Centred on the capsules beside it, which are taller than the caption.
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  const float captionLine = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  caption(draw, ImVec2(x, py + (pillHeight() - captionLine) * 0.5f), "WINDOW");
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  x += ImGui::CalcTextSize("WINDOW").x + 10;
  ImGui::PopFont();
  const bool ram = control_ & IIgsSound::CONTROL_RAM;
  x += pill(draw, ImVec2(x, py), ram ? "RAM" : "REGS", true, ram ? p.blue : p.orange, "REGS") + 4;
  x += pill(draw, ImVec2(x, py), "AUTO+", control_ & IIgsSound::CONTROL_AUTO_INCREMENT, p.green) + 12;
  std::snprintf(value, sizeof value, "$%04X", address_);
  x += field(draw, ImVec2(x, y), "ADDR", value, p.blue, "$FFFF") + 18;
  std::snprintf(value, sizeof value, "%d/15", control_ & IIgsSound::CONTROL_VOLUME_MASK);
  field(draw, ImVec2(x, y), "VOL", value, text(), "15/15");

  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(width, height));
}

// The sound RAM as a waveform, with the table of every running oscillator
// bracketed under it and its playhead. Scroll over it to zoom about the
// pointer, from all 64K down to a few dozen bytes; drag to pan; double-click,
// or Fit, for the whole of it (drawWave).
void EnsoniqWindow::drawRam(float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  ImGuiIO &io = ImGui::GetIO();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float line = ImGui::GetTextLineHeight();
  const int shown = showAll_ ? DOC_OSCILLATORS : std::clamp(enabled_, 1, DOC_OSCILLATORS);
  const float lanes = 10;
  const float scale = ImGui::GetFontSize() * ui::SMALL_TEXT + 4;
  const float header = std::max(line, ImGui::GetFrameHeight());
  const float height = PAD + header + 6 + RAM_HEIGHT + 6 + lanes + scale + PAD;
  card(draw, start, ImVec2(start.x + width, start.y + height));

  const float left = start.x + PAD;
  const float inner = width - PAD * 2;
  float y = start.y + PAD;
  caption(draw, ImVec2(left, y + (header - ImGui::GetFontSize() * ui::SMALL_TEXT) * 0.5f), "SOUND RAM");

  const double total = iigs::SOUND_RAM_SIZE;
  clampView(ramView_, total, RAM_MIN_SPAN);
  char range[48];
  std::snprintf(range, sizeof range, "$%04X-$%04X%s", static_cast<int>(ramView_.start),
                std::min(static_cast<int>(ramView_.start + ramView_.span), 65536) - 1,
                ramView_.span >= total ? "  all 64K" : "");
  zoomControls("ramzoom", draw, left + inner, y, header, ramView_, total, RAM_MIN_SPAN, range);
  y += header + 6;

  const ImVec2 wa(left, y), wb(left + inner, y + RAM_HEIGHT);
  const bool hovered = interactWave("##ram", wa, ImVec2(wb.x, wb.y + lanes), ramView_, total, RAM_MIN_SPAN);
  const double perPixel = ramView_.span / inner;
  auto xFor = [&](double address) { return wa.x + static_cast<float>((address - ramView_.start) / perPixel); };

  draw->AddRectFilled(wa, wb, well(), 6.0f);
  draw->PushClipRect(wa, ImVec2(wb.x, wb.y + lanes + 2), true);
  drawWave(draw, wa, wb, ram_, 0, ramView_, text(0.5f));

  // The tables in use, a lane under the RAM, and where each one is playing.
  const float laneY = wb.y + 4;
  for (int i = 0; i < shown; i++) {
    const Oscillator &o = oscillators_[i];
    if (o.control & IIgsSound::OSC_HALT) continue;
    const float x0 = xFor(o.start);
    const float x1 = xFor(std::min<uint32_t>(o.start + o.length, 65536));
    if (x1 < wa.x || x0 > wb.x) continue;
    const ImU32 colour = hueFor(i);
    const float weight = i == selected_ ? 2.0f : 1.0f;
    draw->AddRectFilled(ImVec2(x0, laneY), ImVec2(std::max(x1, x0 + 2), laneY + 3 * weight), colour, 1.5f);
    draw->AddRectFilled(ImVec2(x0, wa.y), ImVec2(std::max(x1, x0 + 1), wb.y), withAlpha(colour, 0.10f * weight));
    const float px = xFor(o.start + o.position);
    draw->AddLine(ImVec2(px, wa.y + 2), ImVec2(px, wb.y - 2), colour, 1.5f);
  }
  draw->PopClipRect();
  drawTicks(draw, wa.x, wb.x, laneY + lanes - 4, 0, ramView_);

  // What is under the pointer: a byte close in, a page from further out.
  if (hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
    const int at = std::clamp(static_cast<int>(ramView_.start + (io.MousePos.x - wa.x) * perPixel), 0, 65535);
    std::string users;
    for (int i = 0; i < shown; i++) {
      const Oscillator &o = oscillators_[i];
      if (!(o.control & IIgsSound::OSC_HALT) && static_cast<uint32_t>(at) >= o.start &&
          static_cast<uint32_t>(at) < o.start + o.length) {
        char name[8];
        std::snprintf(name, sizeof name, "%s%d", users.empty() ? "" : ", ", i);
        users += name;
      }
    }
    const char *playing = users.empty() ? "" : "\nPlaying from it: ";
    if (perPixel < 16 && ram_.size() == iigs::SOUND_RAM_SIZE) {
      ImGui::SetTooltip("$%04X  $%02X%s%s", at, ram_[at], playing, users.c_str());
    } else {
      ImGui::SetTooltip("$%02X00-$%02XFF%s%s", at >> 8, at >> 8, playing, users.c_str());
    }
  }

  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(width, height));
}

void EnsoniqWindow::drawOscillator(int index, float width, float height) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ui::Palette &p = ui::palette();
  const Oscillator &o = oscillators_[index];
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  const float mid = origin.y + height * 0.5f;
  const bool scanned = index < enabled_;
  const bool running = scanned && !(o.control & IIgsSound::OSC_HALT);
  const bool muted = mutes & (1u << index);
  const float fade = !scanned ? 0.35f : muted ? 0.5f : 1.0f;
  const ImU32 hue = hueFor(index);
  float x = origin.x;

  // Mute, as a button with a speaker on it.
  ImGui::PushID(index);
  ImGui::SetCursorScreenPos(ImVec2(x, mid - 11));
  if (ImGui::InvisibleButton("##mute", ImVec2(22, 22))) setMute(index, !muted);
  const bool hovered = ImGui::IsItemHovered();
  if (hovered) {
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    ImGui::SetTooltip(muted ? "Hear oscillator %d" : "Leave oscillator %d out of the mix", index);
  }
  ImGui::PopID();
  draw->AddRectFilled(ImVec2(x, mid - 11), ImVec2(x + 22, mid + 11),
                      muted ? withAlpha(p.red, 0.22f) : text(hovered ? 0.12f : 0.06f), 6.0f);
  speaker(draw, ImVec2(x + 10, mid), muted, muted ? p.red : secondary());
  x += 30;

  // Its number, filled while it runs.
  char number[4];
  std::snprintf(number, sizeof number, "%02d", index);
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  const ImVec2 ns = ImGui::CalcTextSize(number);
  const ImVec2 ba(x, mid - 10), bb(x + ns.x + 10, mid + 10);
  if (running) draw->AddRectFilled(ba, bb, withAlpha(hue, fade), 5.0f);
  else draw->AddRect(ba, bb, withAlpha(hue, 0.6f * fade), 5.0f, 0, 1.2f);
  draw->AddText(ImVec2(ba.x + 5, mid - ns.y * 0.5f), running ? ui::textOn(hue) : withAlpha(secondary(), fade), number);
  ImGui::PopFont();
  x = bb.x + 8;

  // Mode and interrupt.
  const int mode = (o.control & IIgsSound::OSC_MODE_MASK) >> 1;
  const float py = mid - pillHeight() * 0.5f;
  const float modeX = x;
  x += pill(draw, ImVec2(x, py), MODE_NAMES[mode], running, withAlpha(hue, 0.85f), "SWAP") + 4;
  const bool modeHovered = ui::IsHoveringRect(ImVec2(modeX, py), ImVec2(x, py + pillHeight()));
  const float irqX = x;
  const bool irqEnabled = o.control & IIgsSound::OSC_INTERRUPT_ENABLE;
  x += pill(draw, ImVec2(x, py), "I", irqEnabled, o.interruptPending ? p.red : withAlpha(p.purple, 0.85f)) + 10;
  const bool irqHovered = ui::IsHoveringRect(ImVec2(irqX, py), ImVec2(x, py + pillHeight()));
  if (modeHovered) ImGui::SetTooltip("%s", MODE_TIPS[mode]);
  if (irqHovered) {
    ImGui::SetTooltip("Interrupts when it halts: %s%s", irqEnabled ? "yes" : "no",
                      o.interruptPending ? "\nAn interrupt is waiting to be read from $E0" : "");
  }

  // The pitch the table plays at, taking it as one cycle: a step of the
  // frequency register per scan, and 2^(17 + resolution) of them a table,
  // whatever its size.
  const double hz = sampleRate_ * o.frequency / std::ldexp(1.0, 17 + o.resolution);
  char note[16] = "--";
  char freq[24];
  std::snprintf(freq, sizeof freq, "$%04X", o.frequency);
  if (o.frequency) {
    if (hz >= 16 && hz <= 20000) {
      const int n = static_cast<int>(std::lround(12 * std::log2(hz / 440.0) + 69));
      std::snprintf(note, sizeof note, "%s%d", NOTE_NAMES[((n % 12) + 12) % 12], n / 12 - 1);
    }
    std::snprintf(freq, sizeof freq, hz >= 1000 ? "%.0f Hz" : hz >= 10 ? "%.1f Hz" : "%.2f Hz", hz);
  }
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 1.05f);
  const float noteHeight = ImGui::GetTextLineHeight();
  draw->AddText(ImVec2(x, mid - noteHeight * 0.5f - 6), text(fade), note);
  ImGui::PopFont();
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  draw->AddText(ImVec2(x, mid + 4), withAlpha(secondary(), fade), freq);
  ImGui::PopFont();
  if (ui::IsHoveringRect(ImVec2(x, mid - 14), ImVec2(x + 62, mid + 14))) {
    ImGui::SetTooltip("Frequency $%04X, resolution %d: the table plays %.2f times a second\n"
                      "(its pitch, if it holds one cycle of a wave)",
                      o.frequency, o.resolution, hz);
  }
  x += 66;

  // Volume, 0 to 255.
  const float meter = 46;
  draw->AddRectFilled(ImVec2(x, mid - 3), ImVec2(x + meter, mid + 3), text(0.08f), 3.0f);
  draw->AddRectFilled(ImVec2(x, mid - 3), ImVec2(x + meter * (o.volume / 255.0f), mid + 3), withAlpha(hue, fade), 3.0f);
  char volume[8];
  std::snprintf(volume, sizeof volume, "%d", o.volume);
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  draw->AddText(ImVec2(x, mid - 5 - ImGui::GetTextLineHeight()), withAlpha(secondary(), fade), "VOL");
  draw->AddText(ImVec2(x, mid + 5), withAlpha(text(), fade), volume);
  ImGui::PopFont();
  x += meter + 10;

  // The table itself, from the sound RAM, with the playhead on it.
  const ImVec2 wa(x, origin.y + 2), wb(origin.x + width, origin.y + height - 2);
  draw->AddRectFilled(wa, wb, well(), 6.0f);
  const float w = wb.x - wa.x - 8, h = wb.y - wa.y - 6;
  const float centre = (wa.y + wb.y) * 0.5f;
  draw->AddLine(ImVec2(wa.x + 4, centre), ImVec2(wb.x - 4, centre), text(0.07f));
  if (ram_.size() == iigs::SOUND_RAM_SIZE && o.volume + o.frequency > 0) {
    const int columns = std::max(8, static_cast<int>(w / 2));
    const uint32_t per = std::max<uint32_t>(1, o.length / static_cast<uint32_t>(columns));
    const uint32_t stride = std::max<uint32_t>(1, per / 8);
    const ImU32 colour = withAlpha(hue, running && !muted ? 1.0f : 0.4f);
    for (int c = 0; c < columns; c++) {
      const uint32_t from = static_cast<uint32_t>(c) * o.length / static_cast<uint32_t>(columns);
      int low = 0x80, high = 0x80;
      bool end = false;
      for (uint32_t i = from; i < from + per && i < o.length; i += stride) {
        const int b = ram_[(o.start + i) & 0xFFFF];
        if (!b) {
          end = true;
          continue;
        }
        low = std::min(low, b);
        high = std::max(high, b);
      }
      const float cx = wa.x + 4 + w * (c + 0.5f) / columns;
      if (end) {
        // A zero byte: where the chip stops.
        draw->AddLine(ImVec2(cx, wa.y + 3), ImVec2(cx, wb.y - 3), withAlpha(p.red, 0.5f), 1.0f);
        continue;
      }
      const float yHigh = centre - (high - 0x80) / 128.0f * h * 0.5f;
      const float yLow = centre - (low - 0x80) / 128.0f * h * 0.5f;
      draw->AddLine(ImVec2(cx, yHigh), ImVec2(cx, std::max(yLow, yHigh + 1)), colour, 1.2f);
    }
    if (running) {
      const float px = wa.x + 4 + w * (o.position / static_cast<float>(o.length));
      draw->AddLine(ImVec2(px, wa.y + 2), ImVec2(px, wb.y - 2), text(0.85f), 1.5f);
    }
  }
  char table[32];
  std::snprintf(table, sizeof table, "$%04X  %s", o.start, sizeName(o.length));
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  draw->AddText(ImVec2(wa.x + 6, wa.y + 2), withAlpha(secondary(), fade), table);
  ImGui::PopFont();
  if (ui::IsHoveringRect(wa, wb)) {
    ImGui::SetTooltip("Table $%04X-$%04X, %u bytes, resolution %d\nPlaying byte %u of it; it last read $%02X\n"
                      "Channel %d (only a stereo card would separate them)\nClick to open it in full",
                      o.start, (o.start + o.length - 1) & 0xFFFF, o.length, o.resolution, o.position, o.data,
                      (o.control & IIgsSound::OSC_CHANNEL_MASK) >> 4);
  }

  // Anywhere on the row but its mute button opens it in full.
  const ImVec2 rowA(origin.x + 26, origin.y - 2), rowB(origin.x + width, origin.y + height + 2);
  if (ui::IsHoveringRect(rowA, rowB)) {
    draw->AddRect(rowA, rowB, withAlpha(hue, 0.5f), 7.0f, 0, 1.5f);
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) selected_ = index;
  }

  ImGui::SetCursorScreenPos(origin);
  ImGui::Dummy(ImVec2(width, height));
}

// The oscillators in their pairs, even beside odd: swap and sync both act
// across a pair, so a pair is a row.
void EnsoniqWindow::drawOscillators(float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float line = ImGui::GetTextLineHeight();
  const int shown = showAll_ ? DOC_OSCILLATORS : std::clamp(enabled_ + (enabled_ & 1), 2, DOC_OSCILLATORS);
  const int rows = shown / 2;
  const float height = PAD + line + 6 + rows * (ROW_HEIGHT + 6) - 6 + PAD;
  card(draw, start, ImVec2(start.x + width, start.y + height));

  const float left = start.x + PAD;
  float y = start.y + PAD;
  caption(draw, ImVec2(left, y + 2), "OSCILLATORS");
  y += line + 6;
  const float inner = width - PAD * 2;
  const float gap = 18;
  const float column = (inner - gap) * 0.5f;
  for (int row = 0; row < rows; row++) {
    for (int side = 0; side < 2; side++) {
      ImGui::SetCursorScreenPos(ImVec2(left + side * (column + gap), y));
      drawOscillator(row * 2 + side, column, ROW_HEIGHT);
    }
    // The pair's link.
    const float lx = left + column + gap * 0.5f;
    draw->AddLine(ImVec2(lx - 5, y + ROW_HEIGHT * 0.5f), ImVec2(lx + 5, y + ROW_HEIGHT * 0.5f),
                  withAlpha(hueFor(row * 2), 0.5f), 2.0f);
    y += ROW_HEIGHT + 6;
  }

  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(width, height));
}

// One oscillator in full: what it is doing, every register it has, and its
// table as a wave across the whole card that zooms and pans as the sound RAM
// does, with the playhead on it and, if asked, kept in view.
void EnsoniqWindow::drawOscillatorDetail(int index, float width) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ui::Palette &p = ui::palette();
  const Oscillator &o = oscillators_[index];
  const ImVec2 start = ImGui::GetCursorScreenPos();
  const float line = ImGui::GetTextLineHeight();
  const float frame = ImGui::GetFrameHeight();
  const bool scanned = index < enabled_;
  const bool running = scanned && !(o.control & IIgsSound::OSC_HALT);
  const bool muted = mutes & (1u << index);
  const ImU32 hue = hueFor(index);
  const int mode = (o.control & IIgsSound::OSC_MODE_MASK) >> 1;
  const bool irqEnabled = o.control & IIgsSound::OSC_INTERRUPT_ENABLE;
  const int channel = (o.control & IIgsSound::OSC_CHANNEL_MASK) >> 4;
  const int partner = index ^ 1;

  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * ui::SMALL_TEXT);
  const float smallLine = ImGui::GetTextLineHeight();
  ImGui::PopFont();
  const float rowGap = 12;
  const float fixed = PAD + frame + rowGap + pillHeight() + rowGap + smallLine + rowGap + smallLine * 2 + 6 + rowGap +
                      frame + 6 + smallLine + 6 + PAD;
  const float height = std::max(ImGui::GetContentRegionAvail().y - 2, fixed + 160);
  card(draw, start, ImVec2(start.x + width, start.y + height));
  const float left = start.x + PAD;
  const float right = start.x + width - PAD;
  float y = start.y + PAD;

  // Back to the list, its mute, its number, and the ones either side.
  ImGui::PushID("detail");
  ImGui::SetCursorScreenPos(ImVec2(left, y));
  if (ui::Button("\xE2\x80\xB9  All oscillators") ||
      (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))) {
    selected_ = -1;
  }
  float x = ImGui::GetItemRectMax().x + 14;
  const float mid = y + frame * 0.5f;
  ImGui::SetCursorScreenPos(ImVec2(x, mid - 12));
  if (ImGui::InvisibleButton("##mute", ImVec2(26, 24))) setMute(index, !muted);
  const bool muteHovered = ImGui::IsItemHovered();
  if (muteHovered) ImGui::SetTooltip(muted ? "Hear oscillator %d" : "Leave oscillator %d out of the mix", index);
  draw->AddRectFilled(ImVec2(x, mid - 12), ImVec2(x + 26, mid + 12),
                      muted ? withAlpha(p.red, 0.22f) : text(muteHovered ? 0.12f : 0.06f), 6.0f);
  speaker(draw, ImVec2(x + 12, mid), muted, muted ? p.red : secondary());
  x += 36;

  char number[8];
  std::snprintf(number, sizeof number, "%02d", index);
  ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 1.2f);
  const ImVec2 ns = ImGui::CalcTextSize(number);
  const ImVec2 ba(x, mid - ns.y * 0.5f - 3), bb(x + ns.x + 14, mid + ns.y * 0.5f + 3);
  if (running) draw->AddRectFilled(ba, bb, hue, 6.0f);
  else draw->AddRect(ba, bb, withAlpha(hue, 0.7f), 6.0f, 0, 1.5f);
  draw->AddText(ImVec2(ba.x + 7, mid - ns.y * 0.5f), running ? ui::textOn(hue) : secondary(), number);
  ImGui::PopFont();
  x = bb.x + 12;
  char title[64];
  std::snprintf(title, sizeof title, "Oscillator %d", index);
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.15f);
  const float titleHeight = ImGui::GetTextLineHeight();
  draw->AddText(ImVec2(x, mid - titleHeight * 0.5f), text(), title);
  x += ImGui::CalcTextSize(title).x + 12;
  ImGui::PopFont();
  char pair[64];
  std::snprintf(pair, sizeof pair, "%s of pair %d, with %02d%s", index & 1 ? "odd" : "even", index / 2, partner,
                scanned ? "" : "  \xC2\xB7  not scanned");
  draw->AddText(ImVec2(x, mid - line * 0.5f), secondary(), pair);

  const float arrow = frame * 1.3f;
  ImGui::SetCursorScreenPos(ImVec2(right - 2 * arrow - 4, y));
  ImGui::BeginDisabled(index == 0);
  if (ui::Button("\xE2\x80\xB9##prev", ImVec2(arrow, 0))) selected_ = index - 1;
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The oscillator before");
  ImGui::SameLine(0, 4);
  ImGui::BeginDisabled(index == DOC_OSCILLATORS - 1);
  if (ui::Button("\xE2\x80\xBA##next", ImVec2(arrow, 0))) selected_ = index + 1;
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("The oscillator after");
  y += frame + rowGap;

  // What it is doing.
  const float py = y;
  x = left;
  x += pill(draw, ImVec2(x, py), running ? "RUNNING" : "HALTED", running, running ? p.green : p.red, "RUNNING") + 6;
  x += pill(draw, ImVec2(x, py), MODE_NAMES[mode], running, withAlpha(hue, 0.85f), "SWAP") + 8;
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  const float textY = py + (pillHeight() - ImGui::GetTextLineHeight()) * 0.5f;
  draw->AddText(ImVec2(x, textY), text(0.78f), MODE_TIPS[mode]);
  ImGui::PopFont();
  y += pillHeight() + rowGap;

  // Its readings, as label and value.
  const double hz = sampleRate_ * o.frequency / std::ldexp(1.0, 17 + o.resolution);
  char note[16] = "--";
  if (o.frequency && hz >= 16 && hz <= 20000) {
    const int n = static_cast<int>(std::lround(12 * std::log2(hz / 440.0) + 69));
    std::snprintf(note, sizeof note, "%s%d", NOTE_NAMES[((n % 12) + 12) % 12], n / 12 - 1);
  }
  char value[48];
  x = left;
  std::snprintf(value, sizeof value, "%s", note);
  x += field(draw, ImVec2(x, y), "NOTE", value, ui::accentText(), "C#10") + 22;
  std::snprintf(value, sizeof value, hz >= 1000 ? "%.0f Hz" : hz >= 10 ? "%.1f Hz" : "%.2f Hz", hz);
  x += field(draw, ImVec2(x, y), "PITCH", value, text(), "00000 Hz") + 22;
  std::snprintf(value, sizeof value, "$%04X", o.frequency);
  x += field(draw, ImVec2(x, y), "FREQ", value, p.blue, "$FFFF") + 22;
  std::snprintf(value, sizeof value, "%d", o.resolution);
  x += field(draw, ImVec2(x, y), "RES", value, text(), "7") + 22;
  std::snprintf(value, sizeof value, "%d", o.volume);
  x += field(draw, ImVec2(x, y), "VOL", value, text(), "255") + 8;
  const float meter = 120;
  const float my = y + smallLine * 0.5f;
  draw->AddRectFilled(ImVec2(x, my - 3), ImVec2(x + meter, my + 3), text(0.08f), 3.0f);
  draw->AddRectFilled(ImVec2(x, my - 3), ImVec2(x + meter * (o.volume / 255.0f), my + 3), hue, 3.0f);
  x += meter + 22;
  std::snprintf(value, sizeof value, "%s%s", irqEnabled ? "on" : "off", o.interruptPending ? ", pending" : "");
  x += field(draw, ImVec2(x, y), "IRQ", value, o.interruptPending ? p.red : text(), "off, pending") + 22;
  std::snprintf(value, sizeof value, "%d", channel);
  field(draw, ImVec2(x, y), "CHANNEL", value, text(), "15");
  y += smallLine + 6;

  x = left;
  std::snprintf(value, sizeof value, "$%04X-$%04X", o.start, (o.start + o.length - 1) & 0xFFFF);
  x += field(draw, ImVec2(x, y), "TABLE", value, p.blue, "$0000-$FFFF") + 10;
  x += field(draw, ImVec2(x, y), "", sizeName(o.length), text(), "32K") + 22;
  std::snprintf(value, sizeof value, "$%02X%s", o.pointer, (o.tableSize & IIgsSound::SIZE_BANK) ? ", bank 1" : "");
  x += field(draw, ImVec2(x, y), "POINTER", value, text(), "$FF, bank 1") + 22;
  std::snprintf(value, sizeof value, "%u of %u", o.position, o.length);
  x += field(draw, ImVec2(x, y), "PLAYING", value, text(), "32767 of 32768") + 22;
  std::snprintf(value, sizeof value, "$%06X", o.accumulator & 0xFFFFFF);
  x += field(draw, ImVec2(x, y), "ACC", value, text(), "$FFFFFF") + 22;
  std::snprintf(value, sizeof value, "$%02X", o.data);
  field(draw, ImVec2(x, y), "LAST READ", value, p.purple, "$FF");
  y += smallLine + rowGap;

  // Its seven registers, by their numbers in the chip.
  struct Register {
    const char *name;
    int base;
    int value;
  };
  const Register registers[] = {
      {"FREQ LO", 0x00, o.frequency & 0xFF}, {"FREQ HI", 0x20, o.frequency >> 8}, {"VOL", 0x40, o.volume},
      {"DATA", 0x60, o.data},                {"PTR", 0x80, o.pointer},            {"CTRL", 0xA0, o.control},
      {"SIZE", 0xC0, o.tableSize},
  };
  x = left;
  caption(draw, ImVec2(x, y + 1), "REGISTERS");
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  x += ImGui::CalcTextSize("REGISTERS").x + 16;
  ImGui::PopFont();
  for (const Register &r : registers) {
    char name[24];
    std::snprintf(name, sizeof name, "$%02X %s", r.base + index, r.name);
    std::snprintf(value, sizeof value, "$%02X", r.value);
    x += field(draw, ImVec2(x, y), name, value, text(), "$FF") + 16;
  }
  y += smallLine + rowGap;

  // The table, as a wave: zoom, pan and follow.
  if (tableViewLength_ != o.length || ImGui::IsWindowAppearing()) {
    tableView_ = {};
    tableViewLength_ = o.length;
  }
  const double total = o.length;
  const double minSpan = 16;
  clampView(tableView_, total, minSpan);
  if (follow_ && running) {
    // Kept in view a screen at a time, so the wave holds still to be read
    // between jumps.
    const double at = o.position;
    if (at < tableView_.start || at >= tableView_.start + tableView_.span) {
      tableView_.start = std::clamp(at - tableView_.span * 0.1, 0.0, total - tableView_.span);
    }
  }
  caption(draw, ImVec2(left, y + (frame - ImGui::GetFontSize() * ui::SMALL_TEXT) * 0.5f), "WAVE");
  ImGui::SetCursorScreenPos(ImVec2(left + 70, y));
  ui::Switch("Follow the playhead", &follow_);
  char range[48];
  std::snprintf(range, sizeof range, "$%04X-$%04X%s", (o.start + static_cast<uint32_t>(tableView_.start)) & 0xFFFF,
                (o.start + static_cast<uint32_t>(tableView_.start + tableView_.span) - 1) & 0xFFFF,
                tableView_.span >= total ? "  whole table" : "");
  zoomControls("tablezoom", draw, right, y, frame, tableView_, total, minSpan, range);
  y += frame + 6;

  const float bottom = start.y + height - PAD - smallLine - 6;
  const ImVec2 wa(left, y), wb(right, bottom);
  const bool hovered = interactWave("##table", wa, wb, tableView_, total, minSpan);
  draw->AddRectFilled(wa, wb, well(), 8.0f);
  draw->PushClipRect(wa, wb, true);
  if (o.volume + o.frequency > 0 || running) {
    drawWave(draw, wa, wb, ram_, o.start, tableView_, withAlpha(hue, running && !muted ? 1.0f : 0.5f));
  }
  const double perPixel = tableView_.span / (wb.x - wa.x);
  if (running) {
    const float px = wa.x + static_cast<float>((o.position + 0.5 - tableView_.start) / perPixel);
    draw->AddLine(ImVec2(px, wa.y + 2), ImVec2(px, wb.y - 2), text(0.9f), 2.0f);
  }
  draw->PopClipRect();
  drawTicks(draw, wa.x, wb.x, wb.y + 2, o.start, tableView_);

  if (hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f) && ram_.size() == iigs::SOUND_RAM_SIZE) {
    const uint32_t offset = static_cast<uint32_t>(
        std::clamp(tableView_.start + (ImGui::GetIO().MousePos.x - wa.x) * perPixel, 0.0, total - 1));
    const uint32_t address = (o.start + offset) & 0xFFFF;
    const uint8_t byte = ram_[address];
    ImGui::SetTooltip("$%04X, byte %u of the table: $%02X%s", address, offset, byte,
                      byte ? "" : "\nA zero: the oscillator stops here");
  }
  ImGui::PopID();

  ImGui::SetCursorScreenPos(start);
  ImGui::Dummy(ImVec2(width, height));
}

void EnsoniqWindow::draw(bool *open) {
  if (!open || !*open || !present_) return;
  take();
  ui::BeforeWindow("Ensoniq");
  // The cards are drawn to a fixed width, so only the height is the user's:
  // shorter than its contents, the window scrolls. The width leaves room for
  // the scrollbar, so the cards never sit under it.
  const ImGuiStyle &style = ImGui::GetStyle();
  const float width = WIDTH + style.WindowPadding.x * 2 + style.ScrollbarSize;
  ImGui::SetNextWindowSizeConstraints(ImVec2(width, showOscillators ? 240 : 0), ImVec2(width, FLT_MAX));
  ImGui::SetNextWindowSize(ImVec2(width, 720), ImGuiCond_FirstUseEver);
  // Without the list the window fits the chip and the sound RAM, and with it
  // back it returns to the height it had.
  if (showOscillators && !wasListed_) {
    // A list hidden since the app started has no height to go back to.
    ImGui::SetNextWindowSize(ImVec2(width, restoreHeight_ > 0 ? restoreHeight_ : 720));
    restoreHeight_ = 0;
  }
  wasListed_ = showOscillators;
  const ImGuiWindowFlags flags = showOscillators ? 0 : ImGuiWindowFlags_AlwaysAutoResize;
  const bool listed = showOscillators;
  if (ui::BeginWindow("Ensoniq", open, flags)) {
    if (listed) listedHeight_ = ImGui::GetWindowHeight();
    drawChip(WIDTH);
    ImGui::Dummy(ImVec2(0, 2));
    drawRam(WIDTH);
    if (listed && !showOscillators) restoreHeight_ = listedHeight_;
    if (listed && showOscillators) {
      ImGui::Dummy(ImVec2(0, 2));
      if (selected_ >= 0 && selected_ < DOC_OSCILLATORS) drawOscillatorDetail(selected_, WIDTH);
      else drawOscillators(WIDTH);
    }
  }
  ImGui::End();
}

} // namespace a2e::native
