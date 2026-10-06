/*
 * code_editor.cpp - A source editor drawn with ImGui
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "code_editor.hpp"

#include "ui_theme.hpp"

#include "imgui_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace a2e::native {

namespace {

constexpr float PAD_TOP = 6.0f;
constexpr float TEXT_INSET = 10.0f; // between the gutter and column 0
constexpr size_t UNDO_MAX = 500;
// Typing within this long of the last keystroke is the same step to undo.
constexpr double TYPING_GAP = 1.0;

bool isWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '$' || c == '%' || c == '_'; }

std::vector<std::string> splitLines(const std::string &text) {
  std::vector<std::string> lines(1);
  for (char c : text) {
    if (c == '\r') continue;
    if (c == '\n') lines.emplace_back();
    else lines.back() += c;
  }
  return lines;
}

// Text from outside as the editor can hold it: plain ASCII, quotes straight,
// tabs as spaces, and every line ending (CR, LF or CRLF) a line feed. Other
// control characters are given to the owner to write as text, or dropped
// when it has no way to.
std::string cleaned(const char *text, const std::function<std::string(unsigned char)> &controlText) {
  std::string out;
  if (!text) return out;
  const unsigned char *p = reinterpret_cast<const unsigned char *>(text);
  while (*p) {
    unsigned int c = *p++;
    if (c == '\r') {
      // A listing from an Apple II or an old Mac ends its lines with CR
      // alone, and opened as one long line.
      if (*p == '\n') p++;
      out += '\n';
      continue;
    }
    if (c < 32 && c != '\t' && c != '\n') {
      if (controlText) out += controlText(static_cast<unsigned char>(c));
      continue;
    }
    if (c == 127) {
      if (controlText) out += controlText(127);
      continue;
    }
    if (c >= 0x80) {
      // A UTF-8 sequence: decode it to see whether it is a quote.
      int extra = (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : 0;
      unsigned int cp = c & (0x3F >> extra);
      for (int i = 0; i < extra && *p; i++) cp = (cp << 6) | (*p++ & 0x3F);
      if (cp == 0x2018 || cp == 0x2019) out += '\'';
      else if (cp == 0x201C || cp == 0x201D) out += '"';
      else if (cp == 0x00A0) out += ' ';
      continue;
    }
    if (c == '\t') out += "  ";
    else if (c == '\n' || (c >= 32 && c < 127)) out += static_cast<char>(c);
  }
  return out;
}

} // namespace

// ---------------------------------------------------------------------------
// The text
// ---------------------------------------------------------------------------

void CodeEditor::setText(const std::string &text) {
  lines_ = splitLines(cleaned(text.c_str(), controlText_));
  caret_ = anchor_ = {0, 0};
  undo_.clear();
  redo_.clear();
  lastEdit_ = EditKind::None;
  revision_++;
}

std::string CodeEditor::text() const {
  std::string out;
  for (size_t i = 0; i < lines_.size(); i++) {
    if (i) out += '\n';
    out += lines_[i];
  }
  return out;
}

CodeEditor::Position CodeEditor::clamp(Position p) const {
  p.line = std::clamp(p.line, 0, static_cast<int>(lines_.size()) - 1);
  p.column = std::clamp(p.column, 0, static_cast<int>(lines_[static_cast<size_t>(p.line)].size()));
  return p;
}

CodeEditor::Position CodeEditor::ordered(bool first) const {
  return (caret_ < anchor_) == first ? caret_ : anchor_;
}

void CodeEditor::changed() {
  revision_++;
  scrollToCaret_ = true;
  preferredColumn_ = -1;
}

// Before a change: what the text was, to go back to. Typing and deleting
// keep adding to the step they started until they pause or change kind.
void CodeEditor::record(EditKind kind) {
  const double now = ImGui::GetTime();
  const bool same = kind == lastEdit_ && kind != EditKind::Other && now - lastEditAt_ < TYPING_GAP;
  if (!same) {
    undo_.push_back({lines_, caret_, anchor_});
    if (undo_.size() > UNDO_MAX) undo_.erase(undo_.begin());
  }
  redo_.clear();
  lastEdit_ = kind;
  lastEditAt_ = now;
}

void CodeEditor::eraseRange(Position from, Position to) {
  from = clamp(from);
  to = clamp(to);
  if (to < from) std::swap(from, to);
  std::string &first = lines_[static_cast<size_t>(from.line)];
  if (from.line == to.line) {
    first.erase(static_cast<size_t>(from.column), static_cast<size_t>(to.column - from.column));
    return;
  }
  first = first.substr(0, static_cast<size_t>(from.column)) + lines_[static_cast<size_t>(to.line)].substr(static_cast<size_t>(to.column));
  lines_.erase(lines_.begin() + from.line + 1, lines_.begin() + to.line + 1);
}

void CodeEditor::deleteSelection() {
  if (!hasSelection()) return;
  const Position from = ordered(true), to = ordered(false);
  eraseRange(from, to);
  caret_ = anchor_ = from;
}

void CodeEditor::insert(const std::string &raw) {
  const std::string text = cleaned(raw.c_str(), controlText_);
  if (text.empty() && !hasSelection()) return;
  record(text.size() == 1 && text != "\n" ? EditKind::Typing : EditKind::Other);
  deleteSelection();
  const std::vector<std::string> parts = splitLines(text);
  std::string &line = lines_[static_cast<size_t>(caret_.line)];
  const std::string tail = line.substr(static_cast<size_t>(caret_.column));
  line = line.substr(0, static_cast<size_t>(caret_.column)) + parts[0];
  if (parts.size() == 1) {
    line += tail;
    caret_.column += static_cast<int>(parts[0].size());
  } else {
    for (size_t i = 1; i < parts.size(); i++) {
      lines_.insert(lines_.begin() + caret_.line + static_cast<int>(i), parts[i]);
    }
    caret_.line += static_cast<int>(parts.size()) - 1;
    caret_.column = static_cast<int>(parts.back().size());
    lines_[static_cast<size_t>(caret_.line)] += tail;
  }
  anchor_ = caret_;
  changed();
}

void CodeEditor::replaceOnLine(int from, int to, const std::string &text) {
  record(EditKind::Other);
  std::string &line = lines_[static_cast<size_t>(caret_.line)];
  from = std::clamp(from, 0, static_cast<int>(line.size()));
  to = std::clamp(to, from, static_cast<int>(line.size()));
  line.replace(static_cast<size_t>(from), static_cast<size_t>(to - from), text);
  caret_.column = from + static_cast<int>(text.size());
  anchor_ = caret_;
  changed();
}

void CodeEditor::replaceAll(const std::string &text, std::optional<Position> caret) {
  if (text == this->text()) return;
  record(EditKind::Other);
  lines_ = splitLines(cleaned(text.c_str(), controlText_));
  caret_ = anchor_ = clamp(caret.value_or(caret_));
  changed();
}

void CodeEditor::setCaret(Position at, bool extend) {
  caret_ = clamp(at);
  if (!extend) anchor_ = caret_;
  scrollToCaret_ = true;
}

std::string CodeEditor::selectedText() const {
  if (!hasSelection()) return {};
  const Position from = ordered(true), to = ordered(false);
  std::string out;
  for (int l = from.line; l <= to.line; l++) {
    const std::string &line = lines_[static_cast<size_t>(l)];
    const int a = l == from.line ? from.column : 0;
    const int b = l == to.line ? to.column : static_cast<int>(line.size());
    out += line.substr(static_cast<size_t>(a), static_cast<size_t>(b - a));
    if (l != to.line) out += '\n';
  }
  return out;
}

void CodeEditor::selectAll() {
  anchor_ = {0, 0};
  caret_ = clamp({static_cast<int>(lines_.size()) - 1, 1 << 20});
}

void CodeEditor::undo() {
  if (undo_.empty()) return;
  redo_.push_back({lines_, caret_, anchor_});
  const Snapshot s = undo_.back();
  undo_.pop_back();
  lines_ = s.lines;
  caret_ = s.caret;
  anchor_ = s.anchor;
  lastEdit_ = EditKind::None;
  changed();
}

void CodeEditor::redo() {
  if (redo_.empty()) return;
  undo_.push_back({lines_, caret_, anchor_});
  const Snapshot s = redo_.back();
  redo_.pop_back();
  lines_ = s.lines;
  caret_ = s.caret;
  anchor_ = s.anchor;
  lastEdit_ = EditKind::None;
  changed();
}

void CodeEditor::scrollToLine(int line) { scrollToLine_ = line; }

// A word to the left or right: across spaces, then across a run of word
// characters or of punctuation, and over a line's end onto the next.
CodeEditor::Position CodeEditor::moveWord(Position p, int direction) const {
  const std::string &line = lines_[static_cast<size_t>(p.line)];
  if (direction < 0) {
    if (p.column == 0) return p.line > 0 ? clamp({p.line - 1, 1 << 20}) : p;
    int c = p.column;
    while (c > 0 && line[static_cast<size_t>(c - 1)] == ' ') c--;
    const bool word = c > 0 && isWordChar(line[static_cast<size_t>(c - 1)]);
    while (c > 0 && line[static_cast<size_t>(c - 1)] != ' ' && isWordChar(line[static_cast<size_t>(c - 1)]) == word) c--;
    return {p.line, c};
  }
  if (p.column >= static_cast<int>(line.size())) {
    return p.line + 1 < static_cast<int>(lines_.size()) ? Position{p.line + 1, 0} : p;
  }
  int c = p.column;
  const int n = static_cast<int>(line.size());
  while (c < n && line[static_cast<size_t>(c)] == ' ') c++;
  const bool word = c < n && isWordChar(line[static_cast<size_t>(c)]);
  while (c < n && line[static_cast<size_t>(c)] != ' ' && isWordChar(line[static_cast<size_t>(c)]) == word) c++;
  return {p.line, c};
}

void CodeEditor::selectWordAt(Position p) {
  p = clamp(p);
  const std::string &line = lines_[static_cast<size_t>(p.line)];
  int a = p.column, b = p.column;
  while (a > 0 && isWordChar(line[static_cast<size_t>(a - 1)])) a--;
  while (b < static_cast<int>(line.size()) && isWordChar(line[static_cast<size_t>(b)])) b++;
  anchor_ = {p.line, a};
  caret_ = {p.line, b};
}

// ---------------------------------------------------------------------------
// The keyboard
// ---------------------------------------------------------------------------

void CodeEditor::handleKeys(const Hooks &hooks, bool &edited) {
  ImGuiIO &io = ImGui::GetIO();
  const bool mac = io.ConfigMacOSXBehaviors;
  // On a Mac ImGui reports Command as Ctrl; Option moves by word there, and
  // Ctrl does elsewhere.
  const bool command = io.KeyCtrl;
  const bool word = mac ? io.KeyAlt : io.KeyCtrl;
  const bool shift = io.KeyShift;
  auto pressed = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, true); };

  // The owner's turn first, for the keys it may want.
  for (ImGuiKey k : {ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_Tab, ImGuiKey_Escape, ImGuiKey_UpArrow,
                     ImGuiKey_DownArrow}) {
    if (pressed(k) && hooks.key && hooks.key(k, shift)) {
      io.InputQueueCharacters.resize(0);
      return;
    }
  }

  if (command) {
    if (ImGui::IsKeyPressed(ImGuiKey_A, false)) selectAll();
    if (ImGui::IsKeyPressed(ImGuiKey_C, false) && hasSelection()) ImGui::SetClipboardText(selectedText().c_str());
    if (ImGui::IsKeyPressed(ImGuiKey_X, false) && hasSelection()) {
      ImGui::SetClipboardText(selectedText().c_str());
      record(EditKind::Other);
      deleteSelection();
      changed();
      edited = true;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
      if (const char *clip = ImGui::GetClipboardText(); clip && *clip) {
        insert(clip);
        edited = true;
        if (hooks.pasted) hooks.pasted();
      }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
      if (shift) redo();
      else undo();
      edited = true;
    }
  }

  const Position before = caret_;
  Position to = caret_;
  bool moved = false;
  auto lineLength = [&](int l) { return static_cast<int>(lines_[static_cast<size_t>(l)].size()); };
  auto firstText = [&](int l) {
    const std::string &s = lines_[static_cast<size_t>(l)];
    const size_t c = s.find_first_not_of(' ');
    return c == std::string::npos ? 0 : static_cast<int>(c);
  };
  if (pressed(ImGuiKey_LeftArrow)) {
    moved = true;
    if (hasSelection() && !shift) to = ordered(true);
    else if (mac && command) to.column = to.column == firstText(to.line) ? 0 : firstText(to.line);
    else if (word) to = moveWord(to, -1);
    else if (to.column > 0) to.column--;
    else if (to.line > 0) to = {to.line - 1, lineLength(to.line - 1)};
  }
  if (pressed(ImGuiKey_RightArrow)) {
    moved = true;
    if (hasSelection() && !shift) to = ordered(false);
    else if (mac && command) to.column = lineLength(to.line);
    else if (word) to = moveWord(to, 1);
    else if (to.column < lineLength(to.line)) to.column++;
    else if (to.line + 1 < static_cast<int>(lines_.size())) to = {to.line + 1, 0};
  }
  const int keepColumn = preferredColumn_ >= 0 ? preferredColumn_ : caret_.column;
  auto vertical = [&](int lines) {
    moved = true;
    to.line = std::clamp(to.line + lines, 0, static_cast<int>(lines_.size()) - 1);
    to.column = std::min(keepColumn, lineLength(to.line));
  };
  if (pressed(ImGuiKey_UpArrow)) {
    if (mac && command) to = {0, 0}, moved = true;
    else vertical(-1);
  }
  if (pressed(ImGuiKey_DownArrow)) {
    if (mac && command) to = clamp({1 << 20, 1 << 20}), moved = true;
    else vertical(1);
  }
  if (pressed(ImGuiKey_PageUp)) vertical(-std::max(1, visibleLines_ - 2));
  if (pressed(ImGuiKey_PageDown)) vertical(std::max(1, visibleLines_ - 2));
  if (pressed(ImGuiKey_Home)) to = command ? Position{0, 0} : Position{to.line, firstText(to.line)}, moved = true;
  if (pressed(ImGuiKey_End)) to = command ? clamp({1 << 20, 1 << 20}) : Position{to.line, lineLength(to.line)}, moved = true;
  if (moved) {
    const bool verticalMove = to.line != before.line && (ImGui::IsKeyDown(ImGuiKey_UpArrow) || ImGui::IsKeyDown(ImGuiKey_DownArrow) ||
                                                         ImGui::IsKeyDown(ImGuiKey_PageUp) || ImGui::IsKeyDown(ImGuiKey_PageDown));
    caret_ = clamp(to);
    if (!shift) anchor_ = caret_;
    preferredColumn_ = verticalMove ? keepColumn : -1;
    scrollToCaret_ = true;
    lastEdit_ = EditKind::None;
  }

  if (pressed(ImGuiKey_Backspace)) {
    record(EditKind::Deleting);
    if (hasSelection()) deleteSelection();
    else {
      Position from = caret_;
      if (mac && command) from.column = 0;
      else if (word) from = moveWord(caret_, -1);
      else if (from.column > 0) from.column--;
      else if (from.line > 0) from = {from.line - 1, lineLength(from.line - 1)};
      eraseRange(from, caret_);
      caret_ = anchor_ = from;
    }
    changed();
    edited = true;
  }
  if (pressed(ImGuiKey_Delete)) {
    record(EditKind::Deleting);
    if (hasSelection()) deleteSelection();
    else {
      Position until = caret_;
      if (word) until = moveWord(caret_, 1);
      else if (until.column < lineLength(until.line)) until.column++;
      else if (until.line + 1 < static_cast<int>(lines_.size())) until = {until.line + 1, 0};
      eraseRange(caret_, until);
    }
    changed();
    edited = true;
  }
  if (pressed(ImGuiKey_Enter) || pressed(ImGuiKey_KeypadEnter)) {
    insert("\n");
    edited = true;
  }
  if (pressed(ImGuiKey_Tab)) {
    insert("  ");
    edited = true;
  }
  if (pressed(ImGuiKey_Escape)) anchor_ = caret_;

  // What was typed. Command combinations are the app's.
  std::string typed;
  if (!command) {
    for (ImWchar c : io.InputQueueCharacters) {
      if (c == 0x2018 || c == 0x2019) c = '\'';
      if (c == 0x201C || c == 0x201D) c = '"';
      if (c < 32 || c >= 127) continue;
      if (hooks.acceptsLine) {
        // The caret's line as typing would leave it, over any selection.
        const Position from = ordered(true);
        const Position to = ordered(false);
        const std::string line = lines_[static_cast<size_t>(from.line)].substr(0, static_cast<size_t>(from.column)) +
                                 static_cast<char>(c) +
                                 lines_[static_cast<size_t>(to.line)].substr(static_cast<size_t>(to.column));
        if (!hooks.acceptsLine(line)) continue;
      }
      typed += static_cast<char>(c);
      insert(std::string(1, static_cast<char>(c)));
      edited = true;
    }
  }
  io.InputQueueCharacters.resize(0);
  if (!typed.empty() && hooks.typed) hooks.typed(typed);
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

bool CodeEditor::draw(const char *id, ImVec2 size, float gutterWidth, const Hooks &hooks, const char *placeholder) {
  bool edited = false;
  ImGui::PushFont(ui::monoFont(), 0.0f);
  lineHeight_ = std::floor(ImGui::GetTextLineHeight() + 5);
  charWidth_ = ImGui::CalcTextSize("M").x;

  size_t longest = 0;
  for (const std::string &l : lines_) longest = std::max(longest, l.size());
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild(id, size, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav);
  ImGui::PopStyleVar();
  if (wantFocus_) {
    ImGui::SetWindowFocus();
    wantFocus_ = false;
  }
  ImDrawList *draw = ImGui::GetWindowDrawList();
  ImGuiIO &io = ImGui::GetIO();
  const ImVec2 win = ImGui::GetWindowPos();
  const ImVec2 view = ImGui::GetWindowSize();
  const float scrollX = ImGui::GetScrollX(), scrollY = ImGui::GetScrollY();
  const float textX = win.x + gutterWidth + TEXT_INSET - scrollX;
  const float top = win.y + PAD_TOP - scrollY;
  visibleLines_ = std::max(1, static_cast<int>(view.y / lineHeight_));

  // The whole text is one item: it takes the clicks and gives the window
  // its scrolling extent.
  const ImVec2 extent(std::max(view.x, gutterWidth + TEXT_INSET + (longest + 4) * charWidth_),
                      std::max(view.y, PAD_TOP * 2 + (lines_.size() + 2) * lineHeight_));
  ImGui::SetCursorPos(ImVec2(0, 0));
  ImGui::InvisibleButton("##text", extent);
  const bool itemHovered = ImGui::IsItemHovered();
  focused_ = ImGui::IsWindowFocused();

  auto positionAt = [&](ImVec2 m) {
    Position p;
    p.line = static_cast<int>(std::floor((m.y - top) / lineHeight_));
    p.column = static_cast<int>(std::lround((m.x - textX) / charWidth_));
    return clamp(p);
  };
  const ImVec2 mouse = io.MousePos;
  const bool inGutter = mouse.x < win.x + gutterWidth;
  hovered_.reset();
  if (itemHovered && !inGutter) {
    hovered_ = positionAt(mouse);
    ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
  }

  // The mouse: the gutter is the owner's, Option-click too, and anywhere
  // else places the caret, a drag selecting and a double click a word.
  if (ImGui::IsItemActivated() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const Position at = positionAt(mouse);
    if (inGutter) {
      if (hooks.gutterClicked && mouse.y >= top) {
        const int line = static_cast<int>(std::floor((mouse.y - top) / lineHeight_));
        if (line >= 0 && line < static_cast<int>(lines_.size())) hooks.gutterClicked(line);
      }
    } else if (io.KeyAlt && hooks.optionClicked) {
      hooks.optionClicked(at);
    } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      selectWordAt(at);
    } else {
      setCaret(at, io.KeyShift);
      dragging_ = true;
    }
    lastEdit_ = EditKind::None;
  }
  if (dragging_ && ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
    caret_ = positionAt(mouse);
    if (mouse.y < win.y) ImGui::SetScrollY(scrollY - lineHeight_);
    if (mouse.y > win.y + view.y) ImGui::SetScrollY(scrollY + lineHeight_);
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragging_ = false;

  if (focused_) {
    // Claim the keyboard, or ImGui's Cocoa backend hands every key on to
    // macOS as well, which finds no text view to type into and beeps.
    ImGui::SetNextFrameWantCaptureKeyboard(true);
    handleKeys(hooks, edited);
    // Text input, as an ImGui text field asks for it: the app then sends
    // Command keys here and keeps the machine's keyboard out of it.
    ImGuiContext &g = *GImGui;
    g.PlatformImeData.WantTextInput = true;
    g.PlatformImeData.WantVisible = true;
    g.PlatformImeData.InputPos = caretScreen_;
    g.PlatformImeData.InputLineHeight = lineHeight_;
    g.PlatformImeData.ViewportId = ImGui::GetWindowViewport()->ID;
  }

  // Keep the caret, or the line asked for, in view.
  const float viewHeight = view.y - ImGui::GetStyle().ScrollbarSize;
  if (scrollToCaret_) {
    const float y = PAD_TOP + caret_.line * lineHeight_;
    if (y < scrollY) ImGui::SetScrollY(y - PAD_TOP);
    else if (y + lineHeight_ * 2 > scrollY + viewHeight) ImGui::SetScrollY(y + lineHeight_ * 2 - viewHeight);
    const float x = caret_.column * charWidth_;
    const float textWidth = view.x - gutterWidth - TEXT_INSET - charWidth_ * 2;
    if (x < scrollX) ImGui::SetScrollX(std::max(0.0f, x - charWidth_ * 4));
    else if (x > scrollX + textWidth) ImGui::SetScrollX(x - textWidth + charWidth_ * 4);
    scrollToCaret_ = false;
  }
  if (scrollToLine_) {
    const float y = PAD_TOP + *scrollToLine_ * lineHeight_;
    if (y < scrollY || y + lineHeight_ > scrollY + viewHeight) ImGui::SetScrollY(y - viewHeight * 0.5f);
    scrollToLine_.reset();
  }

  // The lines in view.
  const int first = std::max(0, static_cast<int>((scrollY - PAD_TOP) / lineHeight_));
  const int last = std::min(static_cast<int>(lines_.size()) - 1, first + visibleLines_ + 1);
  const Position selFrom = ordered(true), selTo = ordered(false);
  const ImU32 textColour = ImGui::GetColorU32(ImGuiCol_Text);
  const ImU32 selection = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
  ImFont *font = ImGui::GetFont();
  const float fontSize = ImGui::GetFontSize();
  const float textInset = (lineHeight_ - ImGui::GetTextLineHeight()) * 0.5f;
  const ImVec2 textClipMin(win.x + gutterWidth, win.y), textClipMax(win.x + view.x, win.y + view.y);

  draw->PushClipRect(textClipMin, textClipMax, true);
  for (int i = first; i <= last; i++) {
    const std::string &line = lines_[static_cast<size_t>(i)];
    const float y = top + i * lineHeight_;
    const ImVec2 a(win.x + gutterWidth, y), b(win.x + view.x, y + lineHeight_);
    if (focused_ && i == caret_.line && !hasSelection()) draw->AddRectFilled(a, b, ImGui::GetColorU32(ImGuiCol_Text, 0.045f));
    if (hasSelection() && i >= selFrom.line && i <= selTo.line) {
      const int from = i == selFrom.line ? selFrom.column : 0;
      const int to = i == selTo.line ? selTo.column : static_cast<int>(line.size()) + 1;
      draw->AddRectFilled(ImVec2(textX + from * charWidth_, y), ImVec2(textX + to * charWidth_, y + lineHeight_), selection, 2.0f);
    }
    if (hooks.decorate) hooks.decorate(draw, i, a, b, textX, charWidth_, false);

    std::vector<Run> runs = hooks.colour ? hooks.colour(i, line) : std::vector<Run>{};
    int at = 0;
    auto put = [&](int from, int to, ImU32 colour) {
      if (to <= from) return;
      draw->AddText(font, fontSize, ImVec2(textX + from * charWidth_, y + textInset), colour, line.data() + from,
                    line.data() + to);
    };
    for (const Run &r : runs) {
      put(at, r.start, textColour);
      put(r.start, std::min<int>(r.start + r.length, static_cast<int>(line.size())), r.colour);
      at = r.start + r.length;
    }
    put(at, static_cast<int>(line.size()), textColour);
    if (hooks.decorate) hooks.decorate(draw, i, a, b, textX, charWidth_, true);
  }

  if (empty() && placeholder && !focused_) {
    draw->AddText(font, fontSize, ImVec2(textX, top + textInset), ImGui::GetColorU32(ImGuiCol_TextDisabled), placeholder);
  }

  // The caret, blinking as a Mac's does, and steady while it moves.
  caretScreen_ = ImVec2(textX + caret_.column * charWidth_, top + caret_.line * lineHeight_);
  if (focused_) {
    const double since = ImGui::GetTime() - lastEditAt_;
    const bool on = since < 0.5 || std::fmod(ImGui::GetTime(), 1.06) < 0.6 || !io.ConfigInputTextCursorBlink;
    if (on) {
      draw->AddRectFilled(ImVec2(caretScreen_.x - 1, caretScreen_.y + 2), ImVec2(caretScreen_.x + 1, caretScreen_.y + lineHeight_ - 2),
                          ImGui::GetColorU32(ImGuiCol_CheckMark));
    }
  }
  draw->PopClipRect();

  // The gutter, fixed while the text scrolls across.
  draw->PushClipRect(win, ImVec2(win.x + gutterWidth, win.y + view.y), true);
  draw->AddRectFilled(win, ImVec2(win.x + gutterWidth, win.y + view.y), ImGui::GetColorU32(ImGuiCol_Text, 0.025f));
  if (hooks.gutter) {
    for (int i = first; i <= last; i++) {
      const float y = top + i * lineHeight_;
      hooks.gutter(draw, i, ImVec2(win.x, y), ImVec2(win.x + gutterWidth, y + lineHeight_));
    }
  }
  draw->PopClipRect();
  draw->AddLine(ImVec2(win.x + gutterWidth, win.y), ImVec2(win.x + gutterWidth, win.y + view.y),
                ImGui::GetColorU32(ImGuiCol_Border));

  ImGui::EndChild();
  ImGui::PopFont();
  return edited;
}

} // namespace a2e::native
