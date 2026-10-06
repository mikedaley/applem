/*
 * basic_language.hpp - Applesoft source as the BASIC editor sees it
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace a2e::native::basic {

// The browser's basic-highlighting.js, basic-autocomplete.js and the
// editing half of basic-program-window.js, in C++ and with no ImGui in it,
// so all of it is unit-tested (test_native_basic).
//
// Lines are source text: a line number, then statements. Everything here
// works on the text the user typed, not on a tokenised program; what the
// machine holds is the core's business.

// What a run of characters is, for colouring. The categories are the
// browser's, so a program looks the same in either front end.
enum class Kind : uint8_t {
  Plain,
  LineNumber,
  Flow,        // GOTO GOSUB RETURN IF THEN ON ONERR RESUME END STOP RUN
  Loop,        // FOR TO STEP NEXT
  InputOutput, // PRINT INPUT GET DATA READ RESTORE
  Graphics,    // GR HGR PLOT HPLOT COLOR= HOME HTAB VTAB ...
  Memory,      // PEEK POKE CALL HIMEM: LOMEM: USR DEF FN
  Function,    // SGN INT ABS ... LEFT$ MID$ ...
  Declaration, // DIM LET DEL NEW CLR CLEAR
  Misc,        // REM LOAD SAVE PR# IN# WAIT TRACE ... AND OR NOT &
  Keyword,     // any other token
  String,
  Number,
  Variable,
  Operator,
  Punctuation,
  Comment, // what follows REM
  Error,   // a line number Applesoft cannot have
};

struct Span {
  int start = 0;
  int length = 0;
  Kind kind = Kind::Plain;
};

// The line's spans, in order, covering every character.
std::vector<Span> highlight(std::string_view line);

// The line number at the start of a line, if it has one in Applesoft's range.
std::optional<int> lineNumber(std::string_view line);

// The highest line number Applesoft takes: past it, the line is a ?SYNTAX
// ERROR (Applesoft II BASIC Programming Reference Manual).
constexpr int MAX_LINE_NUMBER = 63999;

// Whether a line's number, if it starts with one, is one Applesoft can have.
// Read by value, so leading zeros are fine and a number of any length is.
bool lineNumberFits(std::string_view line);

// Where each statement of a line is, as character ranges [start, end) of
// the text after the line number. They are split exactly where the core
// counts statements (Emulator::countColonsBetween): at every colon outside
// quotes, and not at all once REM has begun. So a statement index here and a
// statement index from the machine are the same statement.
struct Statement {
  int start = 0;
  int end = 0;
};
std::vector<Statement> statements(std::string_view line);
// The statement that holds a column, or -1 when the column is in the line
// number or there is none.
int statementAt(std::string_view line, int column);

// The browser's Format: blank lines dropped, numbered lines in order and
// numbers right-aligned to the widest, two spaces of indent a FOR level, and
// keywords and names in capitals. Text in quotes, after REM and in DATA is
// left as it was typed, since Applesoft keeps it as it was typed.
std::string format(const std::string &source);

// The browser's Renum: every numbered line renumbered from `first` by
// `step`, in order, and every GOTO, GOSUB (lists included, so ON ... GOTO),
// THEN and RESUME target outside strings and REM rewritten to match. Lines
// with no number are dropped, as the browser drops them. The map from old
// numbers to new is returned so breakpoints can follow.
struct Renumbered {
  std::string source;
  std::map<int, int> mapping;
};
Renumbered renumber(const std::string &source, int first = 10, int step = 10);

// The number a new line after `line` (an index into the lines) takes: ten
// after that line's number, or after the nearest numbered line above it.
int nextLineNumber(const std::vector<std::string> &lines, int line);

// How many FOR levels each line is indented by, as Format indents them.
std::vector<int> indentLevels(const std::vector<std::string> &lines);

// A keyword's entry in the reference the completion list shows.
struct KeywordInfo {
  const char *keyword;
  const char *syntax;
  const char *category; // display name: "Control", "I/O", "Hi-Res"...
  const char *description;
};
const KeywordInfo *keywordInfo(std::string_view keyword);

// What can be typed where the cursor is.
struct Completion {
  enum class Type { Keyword, Variable, LineNumber, Function };
  Type type = Type::Keyword;
  std::string text;     // what is inserted
  std::string category; // shown on the right
  std::string syntax;
  std::string description;
};
// `before` is the line's text up to the cursor; the word being typed is its
// tail. Nothing is offered for a word shorter than two characters, inside a
// string, or after a letter or digit that is not part of it. After GOTO,
// GOSUB, THEN, ON ... GOTO and ONERR GOTO only line numbers are offered,
// after FN only functions, and elsewhere the program's own variables, then
// its functions, then keywords, at most `limit` of them.
std::vector<Completion> complete(const std::string &source, std::string_view before, size_t limit = 12);
// The word being typed at the end of `before`, as complete() reads it.
std::string_view wordBefore(std::string_view before);

// What Run does about the program in the editor and the one in memory. Run
// used to write the editor's program whenever the window had not read or
// written one since it opened, which after every launch replaced a program
// the user had just LOADed with whatever the editor held from last time. It
// now writes only what the user has plainly been working on, and asks when
// the two differ and nothing says which is meant.
struct RunFacts {
  bool editorEmpty = false;
  bool synced = false;         // the editor was read from or written to memory
  bool editedSinceSync = false;
  bool memoryChangedSinceSync = false;
  bool memoryEmpty = false;    // no program in memory at all
  bool memoryMatchesEditor = false; // memory holds the editor's program, tokenised
};
enum class RunChoice { RunMemory, WriteThenRun, Ask };
RunChoice chooseRun(const RunFacts &facts);

// The editor's text for a byte it cannot show, a control character in a
// pasted or opened listing: the same token in braces the core's listing
// writes (basic_control_text.hpp), so it goes back into memory as the byte.
std::string controlText(unsigned char c);

// What an Applesoft error code says, as the ROM prints it.
std::string errorMessage(uint8_t code);

// A real as the variables panel shows it: whole numbers as they are, others
// to nine significant digits, very large or small ones in exponent form.
std::string formatReal(double value);

} // namespace a2e::native::basic
