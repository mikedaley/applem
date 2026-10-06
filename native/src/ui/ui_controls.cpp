/*
 * ui_controls.cpp - Controls drawn the way macOS draws them
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "ui/ui_controls.hpp"

#include "ui/ui_theme.hpp"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <type_traits>
#include <unordered_map>

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

bool VSliderFloat(const char *id, float *value, float min, float max, ImVec2 size, float fillFrom) {
  ImGuiWindow *window = ImGui::GetCurrentWindow();
  if (window->SkipItems) return false;
  const ImGuiID itemId = window->GetID(id);
  const ImVec2 pos = window->DC.CursorPos;
  const ImRect frame(pos, ImVec2(pos.x + size.x, pos.y + size.y));
  ImGui::ItemSize(frame);
  if (!ImGui::ItemAdd(frame, itemId)) return false;

  const float knobRadius = std::round(ImGui::GetFrameHeight() * 0.36f);
  const float trackTop = frame.Min.y + knobRadius;
  const float trackBottom = frame.Max.y - knobRadius;
  bool hovered = false;
  bool held = false;
  ImGui::ButtonBehavior(frame, itemId, &hovered, &held, ImGuiButtonFlags_PressedOnClick);
  bool changed = false;
  if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && *value != fillFrom) {
    *value = fillFrom;
    changed = true;
    ImGui::MarkItemEdited(itemId);
  } else if (held && trackBottom > trackTop) {
    const float t = std::clamp((trackBottom - ImGui::GetIO().MousePos.y) / (trackBottom - trackTop), 0.0f, 1.0f);
    const float next = min + (max - min) * t;
    if (next != *value) {
      *value = next;
      changed = true;
      ImGui::MarkItemEdited(itemId);
    }
  }

  ImDrawList *draw = window->DrawList;
  auto yFor = [&](float v) {
    const float t = max > min ? std::clamp((v - min) / (max - min), 0.0f, 1.0f) : 0.0f;
    return trackBottom - (trackBottom - trackTop) * t;
  };
  const float x = frame.Min.x + size.x * 0.5f;
  const float trackHalf = 2.0f;
  draw->AddRectFilled(ImVec2(x - trackHalf, trackTop), ImVec2(x + trackHalf, trackBottom), track(), trackHalf);
  const float y = yFor(*value);
  const float from = yFor(fillFrom);
  draw->AddRectFilled(ImVec2(x - trackHalf, std::min(y, from)), ImVec2(x + trackHalf, std::max(y, from)), accent(), trackHalf);
  knob(draw, ImVec2(x, y), knobRadius * (held ? 1.06f : 1.0f));
  return changed;
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

// Room left above a window kept on its monitor, for the menu bar and a
// margin, so its title bar can still be reached.
constexpr float TITLE_ROOM = 48.0f;

void KeepOnMonitor(const char *name) {
  float maxHeight = FLT_MAX;
  if (ImGuiWindow *window = ImGui::FindWindowByName(name); window && window->Viewport) {
    const ImGuiPlatformIO &io = ImGui::GetPlatformIO();
    const int monitor = static_cast<ImGuiViewportP *>(window->Viewport)->PlatformMonitor;
    if (monitor >= 0 && monitor < io.Monitors.Size) {
      const ImGuiPlatformMonitor &screen = io.Monitors[monitor];
      maxHeight = screen.WorkSize.y - TITLE_ROOM;
      // No taller than the screen, and moved up when it grew past its
      // bottom: a window low on the screen grows downward, so a cap alone
      // still left the inspector's lower half off the screen.
      const float bottom = screen.WorkPos.y + screen.WorkSize.y;
      const float height = std::min(window->Size.y, maxHeight);
      if (window->Pos.y + height > bottom) {
        const float top = std::max(screen.WorkPos.y + TITLE_ROOM * 0.5f, bottom - height);
        ImGui::SetNextWindowPos(ImVec2(window->Pos.x, top), ImGuiCond_Always);
      }
    }
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0), ImVec2(FLT_MAX, maxHeight));
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

namespace {

// A macOS title bar's measures, in points: its height, and the three
// buttons' size, the first one's centre from the window's left edge, and the
// distance between centres.
constexpr float TITLE_BAR_HEIGHT = 28.0f;
constexpr float LIGHT_DIAMETER = 12.0f;
constexpr float LIGHT_FIRST = 18.0f;
constexpr float LIGHT_PITCH = 20.0f;

// Where a zoomed window was, to go back to.
std::unordered_map<ImGuiID, ImRect> g_unzoomed;

// The window behind under the pointer this frame, which ClickToFocus hides
// from ImGui: its traffic lights still light up for the pointer, as AppKit's
// do on a window that is not the key one.
ImGuiWindow *g_behind = nullptr;

enum class Light { Close, Minimise, Zoom };

void drawLight(ImDrawList *draw, ImVec2 c, Light light, bool active, bool enabled, bool showGlyph, bool held) {
  const float r = LIGHT_DIAMETER * 0.5f;
  // AppKit's own colours for the three, with the darker rim it draws.
  static const ImU32 FILL[3] = {IM_COL32(0xFF, 0x5F, 0x57, 255), IM_COL32(0xFE, 0xBC, 0x2E, 255),
                                IM_COL32(0x28, 0xC8, 0x40, 255)};
  static const ImU32 RIM[3] = {IM_COL32(0xE2, 0x46, 0x3F, 255), IM_COL32(0xE1, 0xA1, 0x16, 255),
                               IM_COL32(0x14, 0xAE, 0x2C, 255)};
  static const ImU32 GLYPH[3] = {IM_COL32(0x4D, 0x00, 0x00, 255), IM_COL32(0x99, 0x57, 0x00, 255),
                                 IM_COL32(0x00, 0x65, 0x00, 255)};
  const int i = static_cast<int>(light);
  if (!active || !enabled) {
    // An inactive window's buttons, and a button that does nothing here, are
    // grey, as AppKit draws them.
    const ImU32 grey = isDark() ? IM_COL32(255, 255, 255, 52) : IM_COL32(0, 0, 0, 40);
    draw->AddCircleFilled(c, r, grey, 24);
    if (!(showGlyph && enabled)) return;
  } else {
    draw->AddCircleFilled(c, r, FILL[i], 24);
    draw->AddCircle(c, r - 0.25f, RIM[i], 24, 0.5f);
    if (held) draw->AddCircleFilled(c, r, IM_COL32(0, 0, 0, 40), 24);
  }
  if (!showGlyph || !enabled) return;
  const ImU32 g = active ? GLYPH[i] : (isDark() ? IM_COL32(255, 255, 255, 170) : IM_COL32(0, 0, 0, 130));
  const float k = r * 0.45f;
  const float t = 1.1f;
  switch (light) {
  case Light::Close:
    draw->AddLine(ImVec2(c.x - k, c.y - k), ImVec2(c.x + k, c.y + k), g, t);
    draw->AddLine(ImVec2(c.x + k, c.y - k), ImVec2(c.x - k, c.y + k), g, t);
    break;
  case Light::Minimise:
    draw->AddLine(ImVec2(c.x - k * 1.2f, c.y), ImVec2(c.x + k * 1.2f, c.y), g, t + 0.2f);
    break;
  case Light::Zoom:
    draw->AddLine(ImVec2(c.x - k * 1.2f, c.y), ImVec2(c.x + k * 1.2f, c.y), g, t + 0.2f);
    draw->AddLine(ImVec2(c.x, c.y - k * 1.2f), ImVec2(c.x, c.y + k * 1.2f), g, t + 0.2f);
    break;
  }
}

// Fill the screen the window is on, or put it back where it was.
void toggleZoom(ImGuiWindow *window) {
  if (auto it = g_unzoomed.find(window->ID); it != g_unzoomed.end()) {
    ImGui::SetWindowPos(window, it->second.Min);
    ImGui::SetWindowSize(window, it->second.GetSize());
    g_unzoomed.erase(it);
    return;
  }
  ImVec2 pos = ImGui::GetMainViewport()->WorkPos, size = ImGui::GetMainViewport()->WorkSize;
  const ImGuiPlatformIO &io = ImGui::GetPlatformIO();
  const int monitor = window->Viewport ? window->Viewport->PlatformMonitor : -1;
  if (monitor >= 0 && monitor < io.Monitors.Size) {
    pos = io.Monitors[monitor].WorkPos;
    size = io.Monitors[monitor].WorkSize;
  }
  g_unzoomed[window->ID] = ImRect(window->Pos, ImVec2(window->Pos.x + window->SizeFull.x, window->Pos.y + window->SizeFull.y));
  ImGui::SetWindowPos(window, pos);
  ImGui::SetWindowSize(window, size);
}

} // namespace

bool BeginWindow(const char *name, bool *open, ImGuiWindowFlags flags) {
  // A docked window is a tab, and closes from it.
  const ImGuiWindow *existing = ImGui::FindWindowByName(name);
  const bool docked = existing && existing->DockIsActive;

  // The title bar is the frame padding either side of a line of text, so the
  // padding is what makes it AppKit's height; it is put back before anything
  // in the window is drawn.
  const float titlePad = std::max(ImGui::GetStyle().FramePadding.y, (TITLE_BAR_HEIGHT - ImGui::GetFontSize()) * 0.5f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, titlePad));
  const bool visible = ImGui::Begin(name, docked ? open : nullptr, flags);
  ImGui::PopStyleVar();

  ImGuiWindow *window = ImGui::GetCurrentWindow();
  if (docked || window->DockIsActive || (flags & ImGuiWindowFlags_NoTitleBar) || window->TitleBarHeight <= 0) {
    return visible;
  }

  ImGuiContext &g = *GImGui;
  const ImRect bar = window->TitleBarRect();
  const bool active = g.NavWindow && g.NavWindow->RootWindow == window->RootWindow;
  const bool canMinimise = !(flags & ImGuiWindowFlags_NoCollapse);
  const bool canZoom = !(flags & (ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoResize));
  const float cy = (bar.Min.y + bar.Max.y) * 0.5f;
  const ImRect group(ImVec2(bar.Min.x + LIGHT_FIRST - LIGHT_PITCH * 0.5f, bar.Min.y),
                     ImVec2(bar.Min.x + LIGHT_FIRST + LIGHT_PITCH * 2.5f, bar.Max.y));
  // macOS shows all three symbols while the pointer is over any of them, and
  // in their colours even on a window that is not the active one.
  const bool underPointer = (g.HoveredWindow && g.HoveredWindow->RootWindow == window->RootWindow) ||
                            g_behind == window->RootWindow;
  const bool overGroup = underPointer && ImGui::IsMouseHoveringRect(group.Min, group.Max, false);

  // The buttons are items in the title bar, as ImGui's own close button is:
  // on the menu layer, never taking keyboard focus, and clipped to the
  // window rather than to its contents.
  const ImGuiItemFlags itemFlags = g.CurrentItemFlags;
  g.CurrentItemFlags |= static_cast<ImGuiItemFlags>(ImGuiItemFlags_NoNavDefaultFocus) | static_cast<ImGuiItemFlags>(ImGuiItemFlags_NoFocus);
  window->DC.NavLayerCurrent = ImGuiNavLayer_Menu;
  ImGui::PushClipRect(window->OuterRectClipped.Min, window->OuterRectClipped.Max, false);
  const Light lights[3] = {Light::Close, Light::Minimise, Light::Zoom};
  const bool enabled[3] = {open != nullptr, canMinimise, canZoom};
  for (int i = 0; i < 3; i++) {
    const ImVec2 c(bar.Min.x + LIGHT_FIRST + LIGHT_PITCH * i, cy);
    const float hit = LIGHT_PITCH * 0.5f;
    const ImRect bb(ImVec2(c.x - hit, c.y - hit), ImVec2(c.x + hit, c.y + hit));
    static const char *const IDS[3] = {"#TL_CLOSE", "#TL_MINIMISE", "#TL_ZOOM"};
    const ImGuiID id = window->GetID(IDS[i]);
    bool hovered = false, held = false;
    bool pressed = false;
    if (ImGui::ItemAdd(bb, id) && enabled[i]) pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held);
    drawLight(window->DrawList, c, lights[i], active || overGroup, enabled[i], overGroup, held);
    if (!pressed) continue;
    switch (lights[i]) {
    case Light::Close: *open = false; break;
    case Light::Minimise: ImGui::SetWindowCollapsed(window, !window->Collapsed); break;
    case Light::Zoom: toggleZoom(window); break;
    }
  }
  ImGui::PopClipRect();
  window->DC.NavLayerCurrent = ImGuiNavLayer_Main;
  g.CurrentItemFlags = itemFlags;
  return visible;
}

void ClickToFocus() {
  ImGuiContext &g = *GImGui;
  ImGuiIO &io = g.IO;
  g_behind = nullptr;
  ImGuiWindow *hovered = g.HoveredWindow;
  if (!hovered || g.OpenPopupStack.Size > 0) return;
  ImGuiWindow *root = hovered->RootWindow;
  if (g.NavWindow && g.NavWindow->RootWindow == root) return;
  // A press let through to a window behind (a traffic light in its title
  // bar) is followed to its release: ImGui's buttons act on the release, and
  // with the window hidden from ImGui the release landed on nothing, so the
  // button never fired.
  if (g.ActiveId != 0 && g.ActiveIdWindow && g.ActiveIdWindow->RootWindow == root) return;

  bool clicked = false;
  for (int b = 0; b < ImGuiMouseButton_COUNT; b++) clicked |= io.MouseClicked[b];
  // A title bar takes the first click, so a window behind can be dragged.
  if (clicked && !(root->Flags & ImGuiWindowFlags_NoTitleBar) && root->TitleBarRect().Contains(io.MousePos)) return;
  // Otherwise nothing in a window behind is hovered: no tooltips, no hover
  // highlights, no scrolling, until it is brought to the front.
  g_behind = root;
  g.HoveredWindow = nullptr;
  g.HoveredWindowUnderMovingWindow = nullptr;
  io.MouseWheel = 0;
  io.MouseWheelH = 0;
  if (!clicked) return;
  ImGui::FocusWindow(hovered);
  // The click is this function's until the button comes up. ImGui reads a
  // click from the button's key state, not from io.MouseClicked, so the
  // button is owned rather than the click cleared: no widget hears it or its
  // release, and neither does ImGui's own "a click on nothing clears focus",
  // which, seeing no hovered window, took the focus straight back.
  static const ImGuiID owner = ImHashStr("ClickToFocus");
  for (int b = 0; b < ImGuiMouseButton_COUNT; b++) {
    if (!io.MouseClicked[b]) continue;
    ImGui::SetKeyOwner(ImGui::MouseButtonToKey(b), owner, ImGuiInputFlags_LockUntilRelease);
  }
}

bool IsHoveringRect(ImVec2 min, ImVec2 max) {
  return ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
         ImGui::IsMouseHoveringRect(min, max);
}

} // namespace a2e::native::ui
