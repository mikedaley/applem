/*
 * drive_ui.hpp - What the 5.25" and 3.5" drive windows draw alike
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "disk_inspector_data.hpp"
#include "disk_platter.hpp"
#include "ui_controls.hpp"
#include "ui_theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace a2e::native {
// The label colour a filename gets, one of eight, as the browser picks it
// (disk_drives.cpp).
uint32_t stickerColor(const std::string &filename);
} // namespace a2e::native

namespace a2e::native::drive_ui {

// A drive's card, the same in both windows, two to a row.
constexpr float CARD_WIDTH = 380;
constexpr float CARD_HEIGHT = 152;
constexpr float CARD_GAP = 12;
constexpr float CARD_PADDING = 14;
constexpr float CARD_ROUNDING = 10;
constexpr float THUMBNAIL_SIZE = 96;
constexpr int THUMBNAIL_PIXELS = 224;
// The two cards and the gap: the width of either window.
constexpr float WINDOW_WIDTH = CARD_WIDTH * 2 + CARD_GAP;

inline ImU32 rgbU32(uint32_t rgb, float alpha = 1.0f) {
  return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF,
                  static_cast<int>(255 * std::clamp(alpha, 0.0f, 1.0f)));
}

inline ImU32 withAlpha(ImU32 colour, float alpha) {
  const int a = static_cast<int>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

inline ImU32 text(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_Text, alpha); }
inline ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }
inline ImU32 accent(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_CheckMark, alpha); }

// The colour of a drive's head and light: green reading, red writing.
inline ImU32 headColour(bool active, bool writing) {
  if (!active) return ui::isDark() ? IM_COL32(150, 150, 156, 255) : IM_COL32(120, 120, 126, 255);
  return writing ? IM_COL32(224, 58, 62, 255) : IM_COL32(97, 187, 70, 255);
}

inline void glow(ImDrawList *draw, ImVec2 centre, float radius, ImU32 colour) {
  for (int i = 4; i >= 1; i--) draw->AddCircleFilled(centre, radius + i * 2.2f, withAlpha(colour, 0.09f));
}

// A light, glowing while lit.
inline void light(ImDrawList *draw, ImVec2 centre, float radius, ImU32 colour, bool lit) {
  if (lit) {
    glow(draw, centre, radius, colour);
    draw->AddCircleFilled(centre, radius, colour);
    draw->AddCircleFilled(ImVec2(centre.x - radius * 0.3f, centre.y - radius * 0.3f), radius * 0.35f,
                          IM_COL32(255, 255, 255, 120));
  } else {
    draw->AddCircleFilled(centre, radius, ui::isDark() ? IM_COL32(70, 70, 74, 255) : IM_COL32(196, 196, 200, 255));
  }
}

// A small capsule with a label; returns its width.
inline float chip(ImDrawList *draw, ImVec2 at, const char *label, ImU32 colour) {
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  const ImVec2 size = ImGui::CalcTextSize(label);
  const ImVec2 end(at.x + size.x + 12, at.y + size.y + 4);
  draw->AddRectFilled(at, end, withAlpha(colour, 0.16f), (end.y - at.y) * 0.5f);
  draw->AddText(ImVec2(at.x + 6, at.y + 2), colour, label);
  ImGui::PopFont();
  return end.x - at.x;
}

inline void centredText(ImDrawList *draw, ImVec2 centre, ImU32 colour, const char *line) {
  const ImVec2 size = ImGui::CalcTextSize(line);
  draw->AddText(ImVec2(std::floor(centre.x - size.x * 0.5f), std::floor(centre.y - size.y * 0.5f)), colour, line);
}

inline void dashedRect(ImDrawList *draw, ImVec2 a, ImVec2 b, ImU32 colour) {
  const float dash = 4.0f;
  const float gap = 3.0f;
  auto line = [&](ImVec2 p, ImVec2 q) {
    const float length = std::hypot(q.x - p.x, q.y - p.y);
    const ImVec2 d((q.x - p.x) / length, (q.y - p.y) / length);
    for (float t = 0; t < length; t += dash + gap) {
      const float u = std::min(t + dash, length);
      draw->AddLine(ImVec2(p.x + d.x * t, p.y + d.y * t), ImVec2(p.x + d.x * u, p.y + d.y * u), colour, 1.0f);
    }
  };
  line(a, ImVec2(b.x, a.y));
  line(ImVec2(b.x, a.y), b);
  line(b, ImVec2(a.x, b.y));
  line(ImVec2(a.x, b.y), a);
}

inline void border(ImDrawList *draw, ImVec2 a, ImVec2 b, float rounding) {
  draw->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), rounding);
}

// A square texture drawn turned about its centre by `angle` radians,
// clockwise on the screen.
inline void drawTurned(ImDrawList *draw, ImTextureID texture, ImVec2 centre, float radius, float angle) {
  const float c = std::cos(angle);
  const float s = std::sin(angle);
  auto corner = [&](float x, float y) {
    return ImVec2(centre.x + (x * c - y * s) * radius, centre.y + (x * s + y * c) * radius);
  };
  draw->AddImageQuad(ImTextureRef(texture), corner(-1, -1), corner(1, -1), corner(1, 1), corner(-1, 1),
                     ImVec2(0, 0), ImVec2(1, 0), ImVec2(1, 1), ImVec2(0, 1));
}

// The light a spinning disk holds still: two soft bands across the medium,
// opposite each other, that do not turn with it.
inline void sheen(ImDrawList *draw, ImVec2 centre, float outer, float inner) {
  for (int side = 0; side < 2; side++) {
    const float middle = -0.75f * static_cast<float>(M_PI) + side * static_cast<float>(M_PI);
    for (int band = 0; band < 3; band++) {
      const float spread = 0.32f - band * 0.09f;
      draw->PathArcTo(centre, outer, middle - spread, middle + spread, 24);
      draw->PathArcTo(centre, inner, middle + spread, middle - spread, 24);
      draw->PathFillConcave(IM_COL32(255, 255, 255, 7));
    }
  }
}

// A disk's outline with nothing in the drive.
inline void ghostDisk(ImDrawList *draw, ImVec2 centre, float radius) {
  const ImU32 line = text(0.16f);
  draw->AddCircle(centre, radius * platter::DISK_EDGE, line, 0, 1.2f);
  draw->AddCircle(centre, radius * platter::HUB_RING_OUTER, line, 0, 1.0f);
  draw->AddCircle(centre, radius * platter::HUB_HOLE, line, 0, 1.0f);
  for (int s = 0; s < 16; s++) {
    const float a = s * 2.0f * static_cast<float>(M_PI) / 16;
    draw->AddLine(ImVec2(centre.x + std::sin(a) * radius * platter::BAND_INNER,
                         centre.y - std::cos(a) * radius * platter::BAND_INNER),
                  ImVec2(centre.x + std::sin(a) * radius * platter::BAND_OUTER,
                         centre.y - std::cos(a) * radius * platter::BAND_OUTER),
                  text(0.06f), 1.0f);
  }
}

/*
 * The turn a disk's picture is drawn at. It is the core's while the core's
 * disk moves; at speed while the motor runs, which covers frames in which
 * nothing read the disk and so the core's did not move; and once the motor
 * stops, coasting down as a spindle does, losing half its speed every
 * `halfLife` seconds, rather than stopping dead.
 */
struct Spin {
  double turn = 0;  // 0 to 1
  double speed = 0; // turns a second
  double at = -1;
  double core = 0;  // the core's turn last frame
  double movedAt = -1;

  void update(double now, double rotation, bool hasDisk, bool motorOn, double turnsPerSecond,
              double halfLife = 0.35) {
    // How long the core's disk may stand still between frames, the machine
    // running a frame at a time, before the picture stops following it.
    constexpr double GAP_SECONDS = 0.06;
    const double dt = at >= 0 ? now - at : 0;
    at = now;
    double moved = rotation - core;
    moved -= std::floor(moved);
    core = rotation;
    auto advance = [&] {
      turn += speed * dt;
      turn -= std::floor(turn);
    };
    if (!hasDisk) {
      speed = 0;
    } else if (moved > 1e-6 && moved < 0.999) {
      turn = rotation;
      speed = turnsPerSecond;
      movedAt = now;
    } else if (dt > 0 && (motorOn || (speed > 0 && now - movedAt < GAP_SECONDS))) {
      speed = turnsPerSecond;
      advance();
    } else if (speed > 0 && dt > 0) {
      speed *= std::pow(0.5, dt / halfLife);
      advance();
      if (speed < 0.02) speed = 0;
    }
  }
  void reset() { *this = Spin{}; }
};

// The disk on a card, turning with the one in the drive; an outline when the
// drive is empty. The head is the track bar's to show, and the inspector's.
inline void thumbnail(ImDrawList *draw, ImVec2 centre, ImTextureID texture, bool present, double spin) {
  const float radius = THUMBNAIL_SIZE * 0.5f;
  if (!present || texture == ImTextureID_Invalid) {
    ghostDisk(draw, centre, radius);
    return;
  }
  draw->AddCircleFilled(ImVec2(centre.x, centre.y + 3), radius * 0.97f, IM_COL32(0, 0, 0, ui::isDark() ? 90 : 40));
  drawTurned(draw, texture, centre, radius, static_cast<float>(-spin * 2 * M_PI));
  sheen(draw, centre, radius * platter::BAND_OUTER, radius * platter::BAND_INNER);
}

/*
 * Where the head is, as a bar the width of `width`: the outermost track at
 * the left, the head a dot sliding along it, lit while the disk turns, and
 * marks at `marks` (fractions of the way across). Under it what the head is
 * on, at the left, and `status` at the right. Returns the height used.
 */
inline float trackBar(ImDrawList *draw, ImVec2 at, float width, bool present, float position,
                      std::initializer_list<float> marks, const std::string &where, const std::string &status,
                      bool active, bool writing) {
  draw->AddRectFilled(at, ImVec2(at.x + width, at.y + 6), text(0.10f), 3.0f);
  for (float mark : marks) {
    const float x = at.x + width * mark;
    draw->AddLine(ImVec2(x, at.y - 1), ImVec2(x, at.y + 7), text(0.25f));
  }
  if (present) {
    const float x = at.x + width * std::clamp(position, 0.0f, 1.0f);
    draw->AddRectFilled(at, ImVec2(x, at.y + 6), withAlpha(active ? headColour(true, writing) : accent(), 0.35f), 3.0f);
    if (active) glow(draw, ImVec2(x, at.y + 3), 3.0f, headColour(true, writing));
    draw->AddCircleFilled(ImVec2(x, at.y + 3), 4.5f, active ? headColour(true, writing) : accent());
  }
  const float y = at.y + 13;
  draw->AddText(ImVec2(at.x, y), present ? text(0.85f) : secondary(), where.c_str());
  const float statusWidth = ImGui::CalcTextSize(status.c_str()).x;
  draw->AddText(ImVec2(at.x + width - statusWidth, y), active ? headColour(true, writing) : secondary(),
                status.c_str());
  return 13 + ImGui::GetTextLineHeight();
}

// A disk's label, the sticker colour its name gives it; a dashed outline
// and `empty` when there is no disk.
inline void label(ImDrawList *draw, ImVec2 a, ImVec2 b, const std::string *filename, const char *empty) {
  const float height = b.y - a.y;
  if (filename) {
    draw->AddRectFilled(a, b, rgbU32(stickerColor(*filename)), 5.0f);
    draw->AddRectFilled(a, ImVec2(a.x + 6, b.y), IM_COL32(0, 0, 0, 30), 5.0f, ImDrawFlags_RoundCornersLeft);
    draw->AddRect(a, b, IM_COL32(0, 0, 0, 40), 5.0f);
    draw->PushClipRect(a, ImVec2(b.x - 8, b.y), true);
    draw->AddText(ImVec2(a.x + 14, a.y + (height - ImGui::GetTextLineHeight()) * 0.5f), IM_COL32(30, 28, 24, 255),
                  filename->c_str());
    draw->PopClipRect();
    if (ui::IsHoveringRect(a, b)) ImGui::SetTooltip("%s", filename->c_str());
  } else {
    dashedRect(draw, a, b, text(0.22f));
    draw->AddText(ImVec2(a.x + 12, a.y + (height - ImGui::GetTextLineHeight()) * 0.5f), secondary(), empty);
  }
}

// A card's frame, and the number of the drive at its top left.
inline void cardFrame(ImDrawList *draw, ImVec2 a, ImVec2 b, int drive, bool picked) {
  draw->AddRectFilled(a, b, ui::isDark() ? IM_COL32(255, 255, 255, 10) : IM_COL32(0, 0, 0, 8), CARD_ROUNDING);
  if (picked) draw->AddRect(a, b, accent(0.9f), CARD_ROUNDING, 0, 1.5f);
  else border(draw, a, b, CARD_ROUNDING);
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * ui::SMALL_TEXT);
  const std::string name = "D" + std::to_string(drive + 1);
  draw->AddText(ImVec2(a.x + 8, a.y + 6), picked ? accent() : secondary(), name.c_str());
  ImGui::PopFont();
}

// A drag of files over a card: where the disk would go.
inline void dropHighlight(ImVec2 a, ImVec2 b, const std::string &prompt) {
  ImDrawList *top = ImGui::GetForegroundDrawList(ImGui::GetWindowViewport());
  top->AddRectFilled(a, b, withAlpha(accent(), 0.16f), CARD_ROUNDING);
  top->AddRect(a, b, accent(), CARD_ROUNDING, 0, 2.5f);
  ImGui::PushFont(nullptr, ImGui::GetFontSize() * 1.15f);
  const ImVec2 size = ImGui::CalcTextSize(prompt.c_str());
  const ImVec2 middle((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
  top->AddRectFilled(ImVec2(middle.x - size.x * 0.5f - 16, middle.y - size.y * 0.5f - 9),
                     ImVec2(middle.x + size.x * 0.5f + 16, middle.y + size.y * 0.5f + 9), accent(), 20.0f);
  top->AddText(ImVec2(middle.x - size.x * 0.5f, middle.y - size.y * 0.5f), IM_COL32_WHITE, prompt.c_str());
  ImGui::PopFont();
}

} // namespace a2e::native::drive_ui
