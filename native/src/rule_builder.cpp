/*
 * rule_builder.cpp - The Condition Rule Builder, for any breakpoint
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "rule_builder.hpp"

#include "ui_controls.hpp"
#include "ui_theme.hpp"

#include "imgui.h"

#include <algorithm>
#include <cfloat>

namespace a2e::native {

namespace {

using ui::Palette;
using ui::palette;

ImU32 text(float alpha = 1.0f) { return ImGui::GetColorU32(ImGuiCol_Text, alpha); }
ImU32 secondary() { return ImGui::GetColorU32(ImGuiCol_TextDisabled); }

ImU32 withAlpha(ImU32 colour, float alpha) {
  const int a = static_cast<int>(((colour >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(alpha, 0.0f, 1.0f));
  return (colour & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

// A small round button with an x, for taking a row away.
bool removeButton(const char *id) {
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const float size = ImGui::GetTextLineHeight();
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const bool pressed = ImGui::InvisibleButton(id, ImVec2(size, size));
  const bool hovered = ImGui::IsItemHovered();
  const ImVec2 c(at.x + size * 0.5f, at.y + size * 0.5f);
  if (hovered) draw->AddCircleFilled(c, size * 0.5f, withAlpha(palette().red, 0.2f));
  const float r = size * 0.2f;
  const ImU32 colour = hovered ? palette().red : secondary();
  draw->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), colour, 1.5f);
  draw->AddLine(ImVec2(c.x + r, c.y - r), ImVec2(c.x - r, c.y + r), colour, 1.5f);
  return pressed;
}

} // namespace

namespace {

// ImGui's text field over a std::string, growing it as it is typed into.
int growString(ImGuiInputTextCallbackData *data) {
  if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
    auto *s = static_cast<std::string *>(data->UserData);
    s->resize(static_cast<size_t>(data->BufTextLen));
    data->Buf = s->data();
  }
  return 0;
}

bool stringField(const char *id, const char *hint, std::string &value, float width) {
  ImGui::SetNextItemWidth(width);
  return ImGui::InputTextWithHint(id, hint, value.data(), value.capacity() + 1, ImGuiInputTextFlags_CallbackResize,
                                  growString, &value);
}

// A pop-up of fixed choices over a string, as a rule's register or flag.
bool choiceField(const char *id, std::string &value, const char *const items[], int count, float width) {
  int current = 0;
  for (int i = 0; i < count; i++) {
    if (value == items[i]) current = i;
  }
  ImGui::SetNextItemWidth(width);
  if (!ui::PopUpButton(id, &current, items, count)) return false;
  value = items[current];
  return true;
}

const char *const RULE_SUBJECTS[] = {"Register", "Flag", "Byte", "Word", "BASIC Var", "BASIC Array"};
// The BASIC window's two, which are the last two above.
constexpr int FIRST_BASIC_SUBJECT = static_cast<int>(ConditionNode::Subject::BasicVar);

} // namespace

// One rule: what is compared, how, and with what.
bool RuleBuilder::drawRule(ConditionNode &rule, const AddressResolver &resolve, bool wide) {
  using Subject = ConditionNode::Subject;
  int subject = static_cast<int>(rule.subject);
  const bool basicOnly = basic_ && subject >= FIRST_BASIC_SUBJECT;
  const int first = basicOnly ? FIRST_BASIC_SUBJECT : 0;
  int shown = subject - first;
  ImGui::SetNextItemWidth(118);
  if (ui::PopUpButton("##subject", &shown, RULE_SUBJECTS + first, IM_ARRAYSIZE(RULE_SUBJECTS) - first)) {
    subject = shown + first;
    const std::string op = rule.op;
    const std::string value = rule.value;
    rule = ConditionNode::rule(static_cast<Subject>(subject));
    rule.op = op;
    rule.value = value;
  }
  ImGui::SameLine(0, 6);
  switch (rule.subject) {
  case Subject::Register: choiceField("##reg", rule.detail, CONDITION_REGISTERS, 6, 76); break;
  case Subject::Flag: choiceField("##flag", rule.detail, CONDITION_FLAGS, 7, 76); break;
  case Subject::Byte:
  case Subject::Word:
    stringField("##address", wide ? "$E1/2000 or name" : "$0024 or name", rule.detail, 140);
    break;
  case Subject::BasicVar: stringField("##var", "I, SC%, A$", rule.detail, 140); break;
  case Subject::BasicArray:
    stringField("##array", "Name", rule.detail, 64);
    ImGui::SameLine(0, 4);
    stringField("##i1", "i1", rule.index1, 34);
    ImGui::SameLine(0, 4);
    stringField("##i2", "i2", rule.index2, 34);
    break;
  }
  ImGui::SameLine(0, 6);
  choiceField("##op", rule.op, CONDITION_OPERATORS, 6, 64);
  ImGui::SameLine(0, 6);
  stringField("##value", rule.subject == Subject::Flag ? "0 or 1" : "$FF or 255", rule.value, 96);

  // What is wrong with it, if anything, beside the button that takes it away.
  const std::string wrong = problem(rule, resolve);
  ImGui::SameLine(0, 6);
  const float size = ImGui::GetTextLineHeight();
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - size) * 0.5f);
  const ImVec2 at = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(size, size));
  if (!wrong.empty()) {
    ImDrawList *draw = ImGui::GetWindowDrawList();
    const ImVec2 c(at.x + size * 0.5f, at.y + size * 0.5f);
    draw->AddCircleFilled(c, size * 0.42f, palette().red);
    draw->AddRectFilled(ImVec2(c.x - 1, c.y - size * 0.24f), ImVec2(c.x + 1, c.y + size * 0.06f), IM_COL32_WHITE);
    draw->AddRectFilled(ImVec2(c.x - 1, c.y + size * 0.14f), ImVec2(c.x + 1, c.y + size * 0.24f), IM_COL32_WHITE);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", wrong.c_str());
  }
  ImGui::SameLine(0, 4);
  const bool remove = removeButton("##removerule");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove the rule");
  return remove;
}

// A group: ALL or ANY of its rules and groups, with a bar down its left in
// a colour for its depth, so nesting reads at a glance. Returns true when
// the group asked to be removed.
bool RuleBuilder::drawGroup(ConditionNode &group, int depth, const AddressResolver &resolve, bool wide) {
  const Palette p = palette();
  const ImU32 colours[] = {p.blue, p.purple, p.orange, p.green, p.red, p.yellow};
  const ImU32 bar = colours[depth % 6];
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 top = ImGui::GetCursorScreenPos();
  constexpr float INDENT = 14.0f;
  ImGui::Indent(INDENT);
  ImGui::BeginGroup();

  bool removeGroup = false;
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Match");
  ImGui::SameLine(0, 6);
  static const char *const LOGIC[] = {"ALL", "ANY"};
  int logic = group.all ? 0 : 1;
  ImGui::SetNextItemWidth(76);
  if (ui::PopUpButton("##logic", &logic, LOGIC, 2)) group.all = logic == 0;
  ImGui::SameLine(0, 6);
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("of the following");
  if (depth > 0) {
    ImGui::SameLine(0, 10);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (ImGui::GetFrameHeight() - ImGui::GetTextLineHeight()) * 0.5f);
    removeGroup = removeButton("##removegroup");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove the group");
  }

  int removeAt = -1;
  for (size_t i = 0; i < group.children.size(); i++) {
    ImGui::PushID(static_cast<int>(i));
    if (i > 0) {
      // The joiner between siblings, small and quiet.
      ImGui::PushFont(ui::monoFont(), ImGui::GetFontSize() * 0.75f);
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(withAlpha(bar, 0.85f)), "%s", group.all ? "AND" : "OR");
      ImGui::PopFont();
    } else {
      ImGui::Dummy(ImVec2(0, 2));
    }
    ConditionNode &child = group.children[i];
    const bool remove = child.type == ConditionNode::Type::Group ? drawGroup(child, depth + 1, resolve, wide)
                                                                  : drawRule(child, resolve, wide);
    if (remove) removeAt = static_cast<int>(i);
    ImGui::PopID();
  }
  if (removeAt >= 0) group.children.erase(group.children.begin() + removeAt);

  ImGui::Dummy(ImVec2(0, 2));
  if (ui::Button("+ Rule", ImVec2(76, 0))) group.children.push_back(newRule());
  ImGui::SameLine(0, 6);
  if (ui::Button("+ Group", ImVec2(76, 0))) {
    ConditionNode inner = ConditionNode::group(!group.all);
    inner.children.push_back(newRule());
    group.children.push_back(std::move(inner));
  }
  ImGui::EndGroup();
  ImGui::Unindent(INDENT);
  const float bottom = ImGui::GetItemRectMax().y;
  draw->AddRectFilled(ImVec2(top.x + 2, top.y + 2), ImVec2(top.x + 5, bottom), withAlpha(bar, 0.9f), 1.5f);
  ImGui::Dummy(ImVec2(0, 4));
  return removeGroup;
}

ConditionNode RuleBuilder::newRule() const {
  return ConditionNode::rule(basic_ ? ConditionNode::Subject::BasicVar : ConditionNode::Subject::Register);
}

void RuleBuilder::open(const std::string &title, const std::string &condition, bool basic) {
  basic_ = basic;
  title_ = title;
  original_ = condition;
  const auto tree = fromExpression(condition);
  replace_ = !tree;
  tree_ = tree ? *tree : ConditionNode::group();
  // Something to fill in, rather than an empty box.
  if (tree_.children.empty()) tree_.children.push_back(newRule());
  openNext_ = true;
}

std::optional<std::string> RuleBuilder::draw(const AddressResolver &resolve, bool wide) {
  if (openNext_) {
    ImGui::OpenPopup("##rulebuilder");
    openNext_ = false;
    open_ = true;
  }
  ImGui::SetNextWindowSize(ImVec2(660, 0));
  if (!ImGui::BeginPopupModal("##rulebuilder", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
    open_ = false;
    return std::nullopt;
  }
  const Palette p = palette();
  ImGui::TextUnformatted(title_.c_str());
  if (replace_) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(p.orange),
                       "The condition \"%s\" cannot be shown as rules. Applying replaces it.", original_.c_str());
  }
  ImGui::Spacing();

  // The rules, scrolling once there are more than fit.
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 60), ImVec2(FLT_MAX, 380));
  ImGui::BeginChild("##rules", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_FrameStyle);
  drawGroup(tree_, 0, resolve, wide);
  ImGui::EndChild();

  // What will be written, and what it means.
  const std::string expression = toExpression(tree_, resolve, wide);
  const std::string wrong = problem(tree_, resolve);
  ImGui::Spacing();
  ImGui::PushFont(ui::monoFont(), 0.0f);
  ImGui::PushTextWrapPos(0);
  if (expression.empty()) ImGui::TextDisabled("(no conditions: the breakpoint always stops)");
  else ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(text(0.8f)), "%s", expression.c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopFont();
  if (!wrong.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(p.red), "%s", wrong.c_str());
  ImGui::Spacing();

  std::optional<std::string> applied;
  if (ui::Button("Clear", ImVec2(90, 0))) tree_ = ConditionNode::group();
  ImGui::SameLine(ImGui::GetContentRegionMax().x - 186);
  if (ui::Button("Cancel", ImVec2(90, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
  ImGui::SameLine(0, 6);
  ImGui::BeginDisabled(!wrong.empty());
  if (ui::Button("Apply", ImVec2(90, 0), ui::ButtonKind::Primary)) {
    applied = expression;
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  ImGui::EndPopup();
  return applied;
}

} // namespace a2e::native
