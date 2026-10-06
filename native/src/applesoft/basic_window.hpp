/*
 * basic_window.hpp - The Applesoft BASIC window: an editor that runs and debugs
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "applesoft/basic_language.hpp"
#include "applesoft/code_editor.hpp"
#include "app/machine_poll.hpp"
#include "app/platform.hpp"
#include "debugger/rule_builder.hpp"

#include "basic/applesoft_vars.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace a2e::native {

class Emulation;

// The browser's Applesoft BASIC window (basic-program-window.js and its
// helpers), drawn natively: an editor for the program, the controls that run
// it, and what it is doing as it runs.
//
// The editor's text and the program in memory are kept apart, as they are in
// the browser: Read takes the program out of memory, Write puts the editor's
// into it, and nothing else moves either way. Unlike the browser, the window
// says when the two differ, and Run writes an edited program before it runs
// it.
//
// Everything that runs is the core's: the breakpoints, statement by
// statement, the condition rules, stepping, the heat map and the runtime
// error are all caught by the emulator at the ROM's statement dispatch
// ($D820), and the window reads what happened once a frame under one lock.
// A breakpoint's own condition is checked here, as the browser checks it:
// the core stops on the line and the window sends it straight back to
// running when the condition is false.
//
// Applesoft only, on the 8-bit machines, as in the browser.
class BasicWindow {
public:
  BasicWindow(Emulation &emulation, Platform &platform);

  // Every frame, open or not: a breakpoint whose condition is false has to
  // be sent back to running even with the window shut.
  void update(bool open);
  bool available() const { return available_; }
  void draw(bool *open);

  void writeSettings(std::string &out) const;
  void readSetting(const char *line);

private:
  // A breakpoint as the user keeps it: a line, a statement of it or the
  // whole of it, or (line -1) a condition rule that stops wherever it
  // becomes true.
  struct Breakpoint {
    int line = 0;
    int statement = -1; // -1: the whole line; a rule's id when line is -1
    bool enabled = true;
    std::string condition;
    uint32_t hits = 0;
    bool isRule() const { return line < 0; }
  };

  // What the machine is doing, read once a frame.
  struct State {
    bool valid = false;
    bool powered = false;
    bool atPrompt = false; // Applesoft's ] prompt
    bool running = false;
    bool paused = false;
    bool hit = false;
    uint16_t breakLine = 0;
    int ruleHit = -1;
    uint16_t curlin = 0xFFFF;
    uint16_t txtptr = 0;
    int statement = 0;
    int statements = 1;
    bool error = false;
    uint16_t errorLine = 0;
    int errorStatement = -1;
    uint8_t errorCode = 0;
    uint32_t programHash = 0; // of the tokenised program, to see it change
  };

  // Polling and running.
  void take();
  void applyBreakpoints();
  void run();
  void startRun();
  void pause();
  void stop();
  void step(bool wholeLine);
  bool read();
  bool write();
  void message(const std::string &text, bool problem = false);

  // Editing.
  void formatNow(bool keepCaretLine);
  void renumberNow();
  bool onKey(ImGuiKey key, bool shift);
  void onTyped(const std::string &typed);
  void updateCompletions();
  void acceptCompletion();
  void toggleBreakpoint(int line, int statement);
  bool lineHasBreakpoint(int line) const;
  const Breakpoint *breakpointAt(int line, int statement) const;
  int textLineFor(int basicLine) const;

  // Drawing.
  void drawToolbar();
  void drawEditor(ImVec2 size);
  void drawSidebar(float width, float height);
  void drawVariables(float width, float height);
  void drawBreakpoints(float width, float height);
  void drawStatusBar();
  void drawCompletions();
  void drawNewConfirm();
  void drawRunConfirm();
  void drawOpenConfirm();
  void openText(const std::string &path, const std::string &text);
  bool editorDirty() const;
  void editValue(uint16_t valueAddress, BasicVarType type, const std::string &text);

  Emulation &emulation_;
  Platform &platform_;
  MachinePoll poll_;
  bool available_ = false;
  bool open_ = false;

  CodeEditor editor_;
  std::string filePath_;
  // The text as it was last read from or written to memory, and the
  // program's hash then: the window says when either side has moved.
  std::optional<std::string> syncedText_;
  uint32_t syncedHash_ = 0;

  State state_;
  bool lastHit_ = false;
  bool lastRunning_ = false;
  bool stepRequested_ = false;
  // The line stopped on or running, and why it stopped.
  std::optional<int> currentLine_;
  int currentStatement_ = -1;
  bool stoppedAtBreak_ = false;
  std::string stopReason_;
  int pulse_ = -2; // the breakpoint list entry flashing, by index; -2 none
  double pulseAt_ = -10.0;

  // A runtime error, which stays until the program is run again or edited
  // away, and follows its line through a renumber.
  struct Error {
    int line = 0;
    int statement = -1;
    std::string message;
    std::string code; // the line's text then, to follow it
  };
  std::optional<Error> error_;

  std::vector<Breakpoint> breakpoints_;
  RuleBuilder rules_;
  int ruleTarget_ = -2; // -1: a new rule; -2: none
  char newLine_[16] = "";
  bool newLineBad_ = false;

  bool trace_ = true;
  bool heat_ = false;
  bool heatApplied_ = false;
  std::map<int, float> heatLevels_;
  double heatAt_ = 0.0;

  // The variables, as last read, and when each value last changed.
  std::vector<BasicVariableInfo> variables_;
  std::vector<BasicArrayInfo> arrays_;
  std::map<std::string, std::string> lastValues_;
  std::map<std::string, double> changedAt_;
  double variablesAt_ = -10.0;
  std::set<std::string> expandedArrays_;
  std::string editingKey_;
  char editText_[256] = "";
  int focusEdit_ = 0; // frames left to ask for the field's focus

  // Completion: the list, which entry is chosen, and the word it replaces.
  std::vector<basic::Completion> completions_;
  bool completionOpen_ = false;
  int completionSelected_ = 0;
  int completionLine_ = 0;
  int completionFrom_ = 0, completionTo_ = 0;
  int lastCaretLine_ = -1;
  uint64_t formattedRevision_ = 0;
  bool formatPending_ = false; // a paste, to be formatted after the editor draws
  bool editorWasFocused_ = false;
  // The settings' text, until the editor is first drawn.
  std::vector<std::string> pendingText_;
  bool haveText_ = false;
  // The machine the breakpoints were given to, by MachineHost::generation:
  // its address cannot say, since a machine built after a switch can be
  // given the address of the one it replaced.
  uint64_t machineGeneration_ = 0;
  double hashedAt_ = -10.0;

  float sidebarWidth_ = 300.0f;
  float breakpointsHeight_ = 200.0f;
  bool confirmNew_ = false;
  bool confirmRun_ = false;
  // A file chosen to open over edits, waiting for the user to say so.
  bool confirmOpen_ = false;
  std::string pendingOpenPath_, pendingOpenText_;
  // The text as last opened, saved, read or written: what Open would lose
  // is whatever differs from it.
  std::string cleanText_;
  std::string message_;
  bool messageProblem_ = false;
  double messageAt_ = -10.0;
};

} // namespace a2e::native
