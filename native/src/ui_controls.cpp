/*
 * ui_controls.cpp - Controls drawn the way macOS draws them
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "ui_controls.hpp"

#include "ui_theme.hpp"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <type_traits>

namespace a2e::native::ui {

namespace {

// The colours AppKit's controls use, from the theme in force.
ImU32 accent(float alpha = 1.0f) {
  ImVec4 colour = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
  colour.w *= alpha * ImGui::GetStyle().Alpha;
  return ImGui::GetColorU32(colour);
}

ImU32 rgba(int r, int g, int b, float alpha) {
  return IM_COL32(r, g, b, static_cast<int>(alpha * 255 * ImGui::GetStyle().Alpha));
}

// A control's face: white in light mode, a translucent grey in dark.
ImU32 face(bool hovered, bool held) {
  if (isDark()) return rgba(255, 255, 255, held ? 0.28f : hovered ? 0.20f : 0.15f);
  return held ? rgba(225, 225, 228, 1.0f) : hovered ? rgba(250, 250, 250, 1.0f) : rgba(255, 255, 255, 1.0f);
}

// The bezel's edge: a hairline in light mode, a faint top highlight in dark.
ImU32 edge() { return isDark() ? rgba(255, 255, 255, 0.08f) : rgba(0, 0, 0, 0.14f); }

// An unselected box's fill and edge, for tick boxes and radio buttons.
ImU32 wellFill() { return isDark() ? rgba(255, 255, 255, 0.10f) : rgba(255, 255, 255, 1.0f); }
ImU32 wellEdge() { return isDark() ? rgba(255, 255, 255, 0.22f) : rgba(0, 0, 0, 0.28f); }

// A track behind a switch or a slider.
ImU32 track() { return isDark() ? rgba(255, 255, 255, 0.16f) : rgba(0, 0, 0, 0.10f); }

// A soft shadow under a raised shape, as AppKit puts under a knob or button.
void shadow(ImDrawList *draw, ImVec2 a, ImVec2 b, float rounding) {
  if (isDark()) return;
  draw->AddRectFilled(ImVec2(a.x, a.y + 0.5f), ImVec2(b.x, b.y + 1.0f), rgba(0, 0, 0, 0.07f), rounding);
}

void knob(ImDrawList *draw, ImVec2 centre, float radius) {
  draw->AddCircleFilled(ImVec2(centre.x, centre.y + 0.6f), radius + 0.4f, rgba(0, 0, 0, 0.22f), 32);
  draw->AddCircleFilled(centre, radius, rgba(255, 255, 255, 1.0f), 32);
  if (!isDark()) draw->AddCircle(centre, radius, rgba(0, 0, 0, 0.12f), 32, 0.75f);
}

// The label's visible part: everything before "##".
const char *labelEnd(const char *label) { return ImGui::FindRenderedTextEnd(label); }

// A small animation per control, toward a target, kept in ImGui's storage.
float animate(ImGuiID id, float target, float speed = 14.0f) {
  float *value = ImGui::GetStateStorage()->GetFloatRef(id, target);
  const float step = ImGui::GetIO().DeltaTime * speed;
  *value = *value < target ? std::min(target, *value + step) : std::max(target, *value - step);
  return *value;
}

void tick(ImDrawList *draw, ImVec2 min, float size, ImU32 colour) {
  const float s = size;
  const ImVec2 points[3] = {ImVec2(min.x + s * 0.24f, min.y + s * 0.52f),
                            ImVec2(min.x + s * 0.43f, min.y + s * 0.71f),
                            ImVec2(min.x + s * 0.77f, min.y + s * 0.30f)};
  draw->AddPolyline(points, 3, colour, ImDrawFlags_None, s * 0.13f);
}

void chevrons(ImDrawList *draw, ImVec2 centre, float size, ImU32 colour) {
  // Small, as AppKit draws them: about a third of the control's height.
  const float w = size * 0.13f;
  const float h = size * 0.10f;
  const float gap = size * 0.06f;
  const ImVec2 up[3] = {ImVec2(centre.x - w, centre.y - gap), ImVec2(centre.x, centre.y - gap - h),
                        ImVec2(centre.x + w, centre.y - gap)};
  const ImVec2 down[3] = {ImVec2(centre.x - w, centre.y + gap), ImVec2(centre.x, centre.y + gap + h),
                          ImVec2(centre.x + w, centre.y + gap)};
  draw->AddPolyline(up, 3, colour, ImDrawFlags_None, 1.5f);
  draw->AddPolyline(down, 3, colour, ImDrawFlags_None, 1.5f);
}

// A label after a control, as ImGui places one.
void labelAfter(ImDrawList *draw, ImVec2 at, const char *label) {
  if (label != labelEnd(label)) {
    draw->AddText(at, ImGui::GetColorU32(ImGuiCol_Text), label, labelEnd(label));
  }
}

} // namespace

bool Button(const char *label, ImVec2 size, ButtonKind kind) {
  ImGuiWindow *window = ImGui::GetCurrentWindow();
  if (window->SkipItems) return false;
  const ImGuiStyle &style = ImGui::GetStyle();
  const ImGuiID id = window->GetID(label);
  const ImVec2 text = ImGui::CalcTextSize(label, nullptr, true);
  const float height = ImGui::GetFrameHeight();
  ImVec2 frame = ImGui::CalcItemSize(size, text.x + height * 1.1f, height);
  const ImVec2 pos = window->DC.CursorPos;
  const ImRect bb(pos, ImVec2(pos.x + frame.x, pos.y + frame.y));
  ImGui::ItemSize(bb, style.FramePadding.y);
  if (!ImGui::ItemAdd(bb, id)) return false;

  bool hovered = false;
  bool held = false;
  const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
  ImDrawList *draw = window->DrawList;
  const float rounding = std::min(frame.y * 0.5f, 999.0f); // a capsule
  ImU32 textColour = ImGui::GetColorU32(ImGuiCol_Text);
  if (kind == ButtonKind::Primary) {
    shadow(draw, bb.Min, bb.Max, rounding);
    draw->AddRectFilled(bb.Min, bb.Max, accent(held ? 0.8f : 1.0f), rounding);
    if (hovered && !held) draw->AddRectFilled(bb.Min, bb.Max, rgba(255, 255, 255, 0.06f), rounding);
    textColour = rgba(255, 255, 255, 1.0f);
  } else {
    shadow(draw, bb.Min, bb.Max, rounding);
    draw->AddRectFilled(bb.Min, bb.Max, face(hovered, held), rounding);
    draw->AddRect(bb.Min, bb.Max, edge(), rounding, 0, 0.75f);
  }
  const ImVec2 at(bb.Min.x + (frame.x - text.x) * 0.5f, bb.Min.y + (frame.y - text.y) * 0.5f);
  draw->AddText(at, textColour, label, labelEnd(label));
  return pressed;
}

bool Checkbox(const char *label, bool *value) {
  ImGuiWindow *window = ImGui::GetCurrentWindow();
  if (window->SkipItems) return false;
  const ImGuiStyle &style = ImGui::GetStyle();
  const ImGuiID id = window->GetID(label);
  const ImVec2 text = ImGui::CalcTextSize(label, nullptr, true);
  const float box = std::round(ImGui::GetFontSize() * 1.05f);
  const float height = ImGui::GetFrameHeight();
  const ImVec2 pos = window->DC.CursorPos;
  const ImRect bb(pos, ImVec2(pos.x + box + (text.x > 0 ? style.ItemInnerSpacing.x + text.x : 0), pos.y + height));
  ImGui::ItemSize(bb, style.FramePadding.y);
  if (!ImGui::ItemAdd(bb, id)) return false;

  bool hovered = false;
  bool held = false;
  const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
  if (pressed) {
    *value = !*value;
    ImGui::MarkItemEdited(id);
  }
  ImDrawList *draw = window->DrawList;
  const ImVec2 min(pos.x, pos.y + (height - box) * 0.5f);
  const ImVec2 max(min.x + box, min.y + box);
  const float rounding = box * 0.26f;
  if (*value) {
    draw->AddRectFilled(min, max, accent(held ? 0.8f : 1.0f), rounding);
    tick(draw, min, box, rgba(255, 255, 255, 1.0f));
  } else {
    shadow(draw, min, max, rounding);
    draw->AddRectFilled(min, max, held ? face(true, true) : wellFill(), rounding);
    draw->AddRect(min, max, wellEdge(), rounding, 0, 0.8f);
  }
  labelAfter(draw, ImVec2(max.x + style.ItemInnerSpacing.x, pos.y + (height - text.y) * 0.5f), label);
  return pressed;
}

bool Switch(const char *label, bool *value) {
  ImGuiWindow *window = ImGui::GetCurrentWindow();
  if (window->SkipItems) return false;
  const ImGuiStyle &style = ImGui::GetStyle();
  const ImGuiID id = window->GetID(label);
  const ImVec2 text = ImGui::CalcTextSize(label, nullptr, true);
  const float height = ImGui::GetFrameHeight();
  const float trackHeight = std::round(height * 0.78f);
  const float trackWidth = std::round(trackHeight * 1.75f);
  const ImVec2 pos = window->DC.CursorPos;
  const float labelWidth = text.x > 0 ? text.x + style.ItemInnerSpacing.x * 2 : 0;
  const ImRect bb(pos, ImVec2(pos.x + labelWidth + trackWidth, pos.y + height));
  ImGui::ItemSize(bb, style.FramePadding.y);
  if (!ImGui::ItemAdd(bb, id)) return false;

  bool hovered = false;
  bool held = false;
  const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
  if (pressed) {
    *value = !*value;
    ImGui::MarkItemEdited(id);
  }
  ImDrawList *draw = window->DrawList;
  labelAfter(draw, ImVec2(pos.x, pos.y + (height - text.y) * 0.5f), label);

  const ImVec2 min(pos.x + labelWidth, pos.y + (height - trackHeight) * 0.5f);
  const ImVec2 max(min.x + trackWidth, min.y + trackHeight);
  const float t = animate(id, *value ? 1.0f : 0.0f);
  const float radius = trackHeight * 0.5f;
  draw->AddRectFilled(min, max, track(), radius);
  if (t > 0) draw->AddRectFilled(min, max, accent(t), radius);
  const float inset = 1.5f;
  const float knobRadius = radius - inset;
  const float x = min.x + radius + (trackWidth - radius * 2) * t;
  knob(draw, ImVec2(x, min.y + radius), knobRadius * (held ? 1.04f : 1.0f));
  return pressed;
}

bool RadioButton(const char *label, bool active) {
  ImGuiWindow *window = ImGui::GetCurrentWindow();
  if (window->SkipItems) return false;
  const ImGuiStyle &style = ImGui::GetStyle();
  const ImGuiID id = window->GetID(label);
  const ImVec2 text = ImGui::CalcTextSize(label, nullptr, true);
  const float box = std::round(ImGui::GetFontSize() * 1.05f);
  const float height = ImGui::GetFrameHeight();
  const ImVec2 pos = window->DC.CursorPos;
  const ImRect bb(pos, ImVec2(pos.x + box + (text.x > 0 ? style.ItemInnerSpacing.x + text.x : 0), pos.y + height));
  ImGui::ItemSize(bb, style.FramePadding.y);
  if (!ImGui::ItemAdd(bb, id)) return false;

  bool hovered = false;
  bool held = false;
  const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
  if (pressed) ImGui::MarkItemEdited(id);
  ImDrawList *draw = window->DrawList;
  const ImVec2 centre(pos.x + box * 0.5f, pos.y + height * 0.5f);
  const float radius = box * 0.5f;
  if (active) {
    draw->AddCircleFilled(centre, radius, accent(held ? 0.8f : 1.0f), 32);
    draw->AddCircleFilled(centre, radius * 0.38f, rgba(255, 255, 255, 1.0f), 24);
  } else {
    draw->AddCircleFilled(centre, radius, held ? face(true, true) : wellFill(), 32);
    draw->AddCircle(centre, radius, wellEdge(), 32, 0.8f);
  }
  labelAfter(draw, ImVec2(pos.x + box + style.ItemInnerSpacing.x, pos.y + (height - text.y) * 0.5f), label);
  return pressed;
}

bool RadioButton(const char *label, int *value, int choice) {
  const bool pressed = RadioButton(label, *value == choice);
  if (pressed) *value = choice;
  return pressed;
}

bool SegmentedControl(const char *id, int *selected, const std::vector<std::string> &labels, float width) {
  ImGuiWindow *window = ImGui::GetCurrentWindow();
  if (window->SkipItems || labels.empty()) return false;
  const ImGuiStyle &style = ImGui::GetStyle();
  ImGui::PushID(id);
  const float height = ImGui::GetFrameHeight();
  float natural = 0;
  for (const std::string &label : labels) natural = std::max(natural, ImGui::CalcTextSize(label.c_str()).x);
  natural = (natural + height) * labels.size();
  const float total = width > 0 ? width : width < 0 ? ImGui::GetContentRegionAvail().x : natural;
  const ImVec2 pos = window->DC.CursorPos;
  const ImRect bb(pos, ImVec2(pos.x + total, pos.y + height));
  ImGui::ItemSize(bb, style.FramePadding.y);
  bool changed = false;
  ImDrawList *draw = window->DrawList;
  const float rounding = height * 0.5f;
  draw->AddRectFilled(bb.Min, bb.Max, track(), rounding);
  const float segment = total / labels.size();
  for (size_t i = 0; i < labels.size(); i++) {
    const ImRect cell(ImVec2(pos.x + segment * i, pos.y), ImVec2(pos.x + segment * (i + 1), pos.y + height));
    const ImGuiID cellId = window->GetID(static_cast<int>(i));
    if (!ImGui::ItemAdd(cell, cellId)) continue;
    bool hovered = false;
    bool held = false;
    if (ImGui::ButtonBehavior(cell, cellId, &hovered, &held) && *selected != static_cast<int>(i)) {
      *selected = static_cast<int>(i);
      changed = true;
    }
    const bool on = *selected == static_cast<int>(i);
    if (on) {
      const ImVec2 a(cell.Min.x + 2, cell.Min.y + 2);
      const ImVec2 b(cell.Max.x - 2, cell.Max.y - 2);
      shadow(draw, a, b, rounding - 2);
      draw->AddRectFilled(a, b, isDark() ? rgba(255, 255, 255, 0.24f) : rgba(255, 255, 255, 1.0f), rounding - 2);
    } else if (hovered) {
      draw->AddRectFilled(ImVec2(cell.Min.x + 2, cell.Min.y + 2), ImVec2(cell.Max.x - 2, cell.Max.y - 2),
                          isDark() ? rgba(255, 255, 255, 0.06f) : rgba(0, 0, 0, 0.04f), rounding - 2);
    }
    const ImVec2 text = ImGui::CalcTextSize(labels[i].c_str());
    draw->AddText(ImVec2(cell.Min.x + (segment - text.x) * 0.5f, cell.Min.y + (height - text.y) * 0.5f),
                  ImGui::GetColorU32(on ? ImGuiCol_Text : ImGuiCol_Text, on ? 1.0f : 0.8f), labels[i].c_str());
  }
  ImGui::PopID();
  return changed;
}

namespace {

// The track, the filled part and the knob; the label after, as ImGui puts
// it, and the value written to the right of the track.
template <typename T>
bool slider(const char *label, T *value, T min, T max, const char *format) {
  ImGuiWindow *window = ImGui::GetCurrentWindow();
  if (window->SkipItems) return false;
  const ImGuiStyle &style = ImGui::GetStyle();
  const ImGuiID id = window->GetID(label);
  const ImVec2 labelSize = ImGui::CalcTextSize(label, nullptr, true);
  const float height = ImGui::GetFrameHeight();
  const float width = ImGui::CalcItemWidth();
  char valueText[64];
  std::snprintf(valueText, sizeof(valueText), format, *value);
  const float valueWidth = ImGui::CalcTextSize("100%").x + style.ItemInnerSpacing.x;
  const ImVec2 pos = window->DC.CursorPos;
  const ImRect frame(pos, ImVec2(pos.x + width, pos.y + height));
  const ImRect bb(pos, ImVec2(frame.Max.x + (labelSize.x > 0 ? style.ItemInnerSpacing.x + labelSize.x : 0), frame.Max.y));
  ImGui::ItemSize(bb, style.FramePadding.y);
  if (!ImGui::ItemAdd(frame, id)) return false;

  const float knobRadius = std::round(height * 0.36f);
  const float trackMin = frame.Min.x + knobRadius;
  const float trackMax = frame.Max.x - valueWidth - knobRadius;
  bool hovered = false;
  bool held = false;
  ImGui::ButtonBehavior(frame, id, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
  bool changed = false;
  if (held && trackMax > trackMin) {
    const float t = std::clamp((ImGui::GetIO().MousePos.x - trackMin) / (trackMax - trackMin), 0.0f, 1.0f);
    T next = static_cast<T>(min + (max - min) * t);
    if constexpr (std::is_integral_v<T>) next = static_cast<T>(std::lround(min + (max - min) * t));
    if (next != *value) {
      *value = next;
      changed = true;
      ImGui::MarkItemEdited(id);
    }
  }

  ImDrawList *draw = window->DrawList;
  const float t = max > min ? std::clamp(static_cast<float>(*value - min) / static_cast<float>(max - min), 0.0f, 1.0f) : 0.0f;
  const float y = frame.Min.y + height * 0.5f;
  const float trackHalf = 2.0f;
  draw->AddRectFilled(ImVec2(trackMin, y - trackHalf), ImVec2(trackMax, y + trackHalf), track(), trackHalf);
  const float x = trackMin + (trackMax - trackMin) * t;
  draw->AddRectFilled(ImVec2(trackMin, y - trackHalf), ImVec2(x, y + trackHalf), accent(), trackHalf);
  knob(draw, ImVec2(x, y), knobRadius * (held ? 1.06f : 1.0f));

  std::snprintf(valueText, sizeof(valueText), format, *value);
  const ImVec2 valueSize = ImGui::CalcTextSize(valueText);
  draw->AddText(ImVec2(frame.Max.x - valueSize.x, frame.Min.y + (height - valueSize.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), valueText);
  labelAfter(draw, ImVec2(frame.Max.x + style.ItemInnerSpacing.x, frame.Min.y + (height - labelSize.y) * 0.5f), label);
  return changed;
}

} // namespace

bool SliderInt(const char *label, int *value, int min, int max, const char *format) {
  return slider(label, value, min, max, format);
}

bool SliderFloat(const char *label, float *value, float min, float max, const char *format) {
  return slider(label, value, min, max, format);
}

bool BeginPopUpButton(const char *label, const char *preview, float width) {
  ImGuiWindow *window = ImGui::GetCurrentWindow();
  if (window->SkipItems) return false;
  const ImGuiStyle &style = ImGui::GetStyle();
  const ImGuiID id = window->GetID(label);
  const ImGuiID popupId = ImHashStr("##ComboPopup", 0, id);
  const ImVec2 labelSize = ImGui::CalcTextSize(label, nullptr, true);
  const float height = ImGui::GetFrameHeight();
  const float frameWidth = width != 0 ? (width < 0 ? ImGui::GetContentRegionAvail().x + width + 1 : width)
                                      : ImGui::CalcItemWidth();
  const ImVec2 pos = window->DC.CursorPos;
  const ImRect frame(pos, ImVec2(pos.x + frameWidth, pos.y + height));
  const ImRect bb(pos, ImVec2(frame.Max.x + (labelSize.x > 0 ? style.ItemInnerSpacing.x + labelSize.x : 0), frame.Max.y));
  ImGui::ItemSize(bb, style.FramePadding.y);
  if (!ImGui::ItemAdd(frame, id)) return false;

  bool hovered = false;
  bool held = false;
  const bool pressed = ImGui::ButtonBehavior(frame, id, &hovered, &held);
  const bool open = ImGui::IsPopupOpen(popupId, ImGuiPopupFlags_None);
  if (pressed && !open) {
    ImGui::OpenPopupEx(popupId, ImGuiPopupFlags_None);
  }

  ImDrawList *draw = window->DrawList;
  const float rounding = height * 0.5f;
  shadow(draw, frame.Min, frame.Max, rounding);
  draw->AddRectFilled(frame.Min, frame.Max, face(hovered, held || open), rounding);
  draw->AddRect(frame.Min, frame.Max, edge(), rounding, 0, 0.75f);
  const float chevronArea = height * 0.8f;
  ImGui::PushClipRect(frame.Min, ImVec2(frame.Max.x - chevronArea, frame.Max.y), true);
  draw->AddText(ImVec2(frame.Min.x + style.FramePadding.x + 4, frame.Min.y + style.FramePadding.y),
                ImGui::GetColorU32(ImGuiCol_Text), preview);
  ImGui::PopClipRect();
  chevrons(draw, ImVec2(frame.Max.x - chevronArea * 0.6f, frame.Min.y + height * 0.5f), height,
           ImGui::GetColorU32(ImGuiCol_Text, 0.75f));
  labelAfter(draw, ImVec2(frame.Max.x + style.ItemInnerSpacing.x, frame.Min.y + style.FramePadding.y), label);

  if (!ImGui::IsPopupOpen(popupId, ImGuiPopupFlags_None)) return false;
  return ImGui::BeginComboPopup(popupId, frame, ImGuiComboFlags_None);
}

void EndPopUpButton() { ImGui::EndCombo(); }

bool PopUpButton(const char *label, int *current, const char *const items[], int count) {
  const char *preview = *current >= 0 && *current < count ? items[*current] : "";
  bool changed = false;
  if (BeginPopUpButton(label, preview)) {
    for (int i = 0; i < count; i++) {
      if (ImGui::Selectable(items[i], *current == i) && *current != i) {
        *current = i;
        changed = true;
      }
    }
    EndPopUpButton();
  }
  return changed;
}

bool Disclosure(const char *label, bool defaultOpen) {
  ImGuiWindow *window = ImGui::GetCurrentWindow();
  if (window->SkipItems) return false;
  const ImGuiStyle &style = ImGui::GetStyle();
  const ImGuiID id = window->GetID(label);
  ImGuiStorage *storage = ImGui::GetStateStorage();
  bool open = storage->GetBool(id, defaultOpen);

  const float height = ImGui::GetFrameHeight();
  const ImVec2 labelSize = ImGui::CalcTextSize(label, nullptr, true);
  const ImVec2 pos = window->DC.CursorPos;
  const float arrow = height * 0.7f;
  const ImRect bb(pos, ImVec2(pos.x + arrow + style.ItemInnerSpacing.x + labelSize.x, pos.y + height));
  ImGui::ItemSize(bb, style.FramePadding.y);
  if (!ImGui::ItemAdd(bb, id)) return open;

  bool hovered = false;
  bool held = false;
  if (ImGui::ButtonBehavior(bb, id, &hovered, &held)) {
    open = !open;
    storage->SetBool(id, open);
  }

  // A chevron that turns from pointing right to pointing down, as AppKit's
  // disclosure triangle has since Big Sur.
  const float turn = animate(id + 1, open ? 1.0f : 0.0f) * IM_PI * 0.5f;
  const ImVec2 centre(pos.x + arrow * 0.5f, pos.y + height * 0.5f);
  const float s = height * 0.14f;
  const float c = std::cos(turn);
  const float n = std::sin(turn);
  auto point = [&](float x, float y) { return ImVec2(centre.x + x * c - y * n, centre.y + x * n + y * c); };
  const ImVec2 chevron[3] = {point(-s * 0.5f, -s), point(s * 0.5f, 0), point(-s * 0.5f, s)};
  ImDrawList *draw = window->DrawList;
  draw->AddPolyline(chevron, 3, ImGui::GetColorU32(ImGuiCol_Text, hovered ? 1.0f : 0.7f), ImDrawFlags_None, 1.6f);
  draw->AddText(ImVec2(pos.x + arrow + style.ItemInnerSpacing.x, pos.y + style.FramePadding.y),
                ImGui::GetColorU32(ImGuiCol_Text), label, labelEnd(label));
  return open;
}

bool Disclosure(const char *label, bool *open) {
  ImGuiWindow *window = ImGui::GetCurrentWindow();
  if (window->SkipItems) return false;
  ImGui::GetStateStorage()->SetBool(window->GetID(label), *open);
  const bool now = Disclosure(label, *open);
  const bool changed = now != *open;
  *open = now;
  return changed;
}

float SwitchWidth(const char *label) {
  const float trackWidth = std::round(std::round(ImGui::GetFrameHeight() * 0.78f) * 1.75f);
  const float text = ImGui::CalcTextSize(label, nullptr, true).x;
  return (text > 0 ? text + ImGui::GetStyle().ItemInnerSpacing.x * 2 : 0) + trackWidth;
}

namespace {
bool g_windowDocking = false;
} // namespace

void SetWindowDocking(bool allowed) { g_windowDocking = allowed; }

void DialogAnchor::note() {
  const ImVec2 pos = ImGui::GetWindowPos();
  const ImVec2 size = ImGui::GetWindowSize();
  centre_ = ImVec2(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
  frame_ = ImGui::GetFrameCount();
}

void DialogAnchor::placeNext() const {
  // Drawn this frame or the last: the window is showing.
  const bool showing = frame_ >= 0 && ImGui::GetFrameCount() - frame_ <= 1;
  ImGui::SetNextWindowPos(showing ? centre_ : ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                          ImVec2(0.5f, 0.5f));
}

void BeforeWindow(const char *name) {
  if (g_windowDocking) return;
  ImGuiWindowClass own;
  own.ClassId = ImHashStr(name);
  own.DockingAllowUnclassed = false;
  ImGui::SetNextWindowClass(&own);
  if (ImGuiWindow *window = ImGui::FindWindowByName(name); window && window->DockId != 0) {
    ImGui::SetNextWindowDockID(0, ImGuiCond_Always);
  }
}

} // namespace a2e::native::ui
