/*
 * code_editor.hpp - A source editor drawn with ImGui
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "imgui.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace a2e::native {

// ImGui's multiline text box has no colours, no gutter and nothing to mark
// a statement with, so the BASIC editor draws its own. This is the editing
// half: lines of ASCII text, a caret and a selection, undo, the clipboard,
// and the keys a Mac user expects of a text view (Option moves by word,
// Command to the line's ends and the document's, Shift extends). What the
// text means is the owner's: it colours each line, draws the gutter, and
// decorates a line behind and in front of the text through the hooks.
//
// While it has the keyboard it says it wants text input, as an ImGui text
// field does, so the app sends it Command keys and leaves the machine
// alone.
class CodeEditor {
public:
  struct Position {
    int line = 0;
    int column = 0;
    bool operator==(const Position &o) const { return line == o.line && column == o.column; }
    bool operator!=(const Position &o) const { return !(*this == o); }
    bool operator<(const Position &o) const { return line < o.line || (line == o.line && column < o.column); }
  };

  struct Run {
    int start = 0;
    int length = 0;
    ImU32 colour = 0;
  };

  struct Hooks {
    // A line's colours, covering what is to be coloured; the rest is drawn
    // in the text colour.
    std::function<std::vector<Run>(int line, const std::string &text)> colour;
    // The gutter for a line, in [a, b).
    std::function<void(ImDrawList *, int line, ImVec2 a, ImVec2 b)> gutter;
    // A line's decorations: behind the text first, then in front of it.
    // `textX` is where column 0 is drawn and `charWidth` a column's width.
    std::function<void(ImDrawList *, int line, ImVec2 a, ImVec2 b, float textX, float charWidth, bool front)> decorate;
    std::function<void(int line)> gutterClicked;
    std::function<void(Position)> optionClicked;
    // Offered every key press first: true when the owner has dealt with it.
    std::function<bool(ImGuiKey key, bool shift)> key;
    // Asked before a typed character changes a line, with the line as it
    // would be: false refuses the character. A language whose lines have
    // rules (Applesoft's numbers stop at 63999) keeps them as they are typed.
    std::function<bool(const std::string &line)> acceptsLine;
    // After the text changed by typing, with what was typed.
    std::function<void(const std::string &typed)> typed;
    // After a paste (Command-V) changed the text.
    std::function<void()> pasted;
  };

  // How a control character arriving from outside (a file, the clipboard)
  // is written into the text. With none set it is dropped.
  void setControlText(std::function<std::string(unsigned char)> controlText) { controlText_ = std::move(controlText); }

  void setText(const std::string &text);
  std::string text() const;
  const std::vector<std::string> &lines() const { return lines_; }
  bool empty() const { return lines_.size() == 1 && lines_[0].empty(); }

  // Replace everything as one step the user can undo, keeping the caret
  // where it was as nearly as the new text allows.
  void replaceAll(const std::string &text, std::optional<Position> caret = std::nullopt);
  // Type or paste at the caret, over any selection.
  void insert(const std::string &text);
  // Replace the columns [from, to) of the caret's line, as completion does.
  void replaceOnLine(int from, int to, const std::string &text);

  Position caret() const { return caret_; }
  void setCaret(Position at, bool extend = false);
  bool hasSelection() const { return anchor_ != caret_; }
  std::string selectedText() const;
  void selectAll();

  bool canUndo() const { return !undo_.empty(); }
  bool canRedo() const { return !redo_.empty(); }
  void undo();
  void redo();

  // Bring a line into view, in the middle when it is far off.
  void scrollToLine(int line);
  void focus() { wantFocus_ = true; }
  bool focused() const { return focused_; }

  // Counts every change to the text, for whoever caches what it read.
  uint64_t revision() const { return revision_; }
  // The text position under the pointer as of this frame's draw.
  std::optional<Position> hovered() const { return hovered_; }
  // Where the caret is on the screen, for a popup to open beside.
  ImVec2 caretScreenPos() const { return caretScreen_; }
  float lineHeight() const { return lineHeight_; }

  // The editor at the cursor, `size` big, with a gutter `gutterWidth` wide.
  // Returns true when the text changed this frame.
  bool draw(const char *id, ImVec2 size, float gutterWidth, const Hooks &hooks, const char *placeholder = nullptr);

private:
  struct Snapshot {
    std::vector<std::string> lines;
    Position caret, anchor;
  };
  enum class EditKind { None, Typing, Deleting, Other };

  Position clamp(Position p) const;
  Position ordered(bool first) const;
  void record(EditKind kind);
  void deleteSelection();
  void eraseRange(Position from, Position to);
  void handleKeys(const Hooks &hooks, bool &changed);
  Position moveWord(Position p, int direction) const;
  void selectWordAt(Position p);
  void changed();

  std::vector<std::string> lines_{""};
  Position caret_, anchor_;
  int preferredColumn_ = -1;
  std::vector<Snapshot> undo_, redo_;
  EditKind lastEdit_ = EditKind::None;
  double lastEditAt_ = -10.0;
  uint64_t revision_ = 0;
  std::function<std::string(unsigned char)> controlText_;

  bool focused_ = false;
  bool wantFocus_ = false;
  bool scrollToCaret_ = false;
  std::optional<int> scrollToLine_;
  bool dragging_ = false;
  std::optional<Position> hovered_;
  ImVec2 caretScreen_{0, 0};
  float lineHeight_ = 18.0f;
  float charWidth_ = 7.0f;
  int visibleLines_ = 20;
};

} // namespace a2e::native
