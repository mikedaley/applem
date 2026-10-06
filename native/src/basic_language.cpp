/*
 * basic_language.cpp - Applesoft source as the BASIC editor sees it
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "basic_language.hpp"

#include "basic/basic_control_text.hpp"
#include "basic/basic_tokens.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <regex>
#include <set>

namespace a2e::native::basic {

namespace {

char upper(char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
bool isAlpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
bool isAlnum(char c) { return isAlpha(c) || isDigit(c); }

std::string uppercase(std::string_view s) {
  std::string out(s);
  for (char &c : out) c = upper(c);
  return out;
}

// The browser's categories, keyword by keyword.
struct Category {
  Kind kind;
  std::vector<const char *> words;
};
const std::vector<Category> &categories() {
  static const std::vector<Category> list = {
      {Kind::Flow, {"GOTO", "GOSUB", "RETURN", "IF", "THEN", "ON", "ONERR", "RESUME", "END", "STOP", "RUN"}},
      {Kind::Loop, {"FOR", "TO", "STEP", "NEXT"}},
      {Kind::InputOutput, {"PRINT", "INPUT", "GET", "DATA", "READ", "RESTORE"}},
      {Kind::Graphics, {"GR", "HGR", "HGR2", "TEXT", "PLOT", "HPLOT", "HLIN", "VLIN", "COLOR=", "HCOLOR=", "DRAW",
                        "XDRAW", "ROT=", "SCALE=", "SCRN(", "HOME", "HTAB", "VTAB", "NORMAL", "INVERSE", "FLASH"}},
      {Kind::Memory, {"PEEK", "POKE", "CALL", "HIMEM:", "LOMEM:", "USR", "DEF", "FN"}},
      {Kind::Function, {"SGN", "INT", "ABS", "SQR", "RND", "LOG", "EXP", "COS", "SIN", "TAN", "ATN", "LEN", "ASC",
                        "VAL", "STR$", "CHR$", "LEFT$", "RIGHT$", "MID$", "FRE", "PDL", "POS", "TAB(", "SPC("}},
      {Kind::Declaration, {"DIM", "LET", "DEL", "NEW", "CLR", "CLEAR"}},
      {Kind::Misc, {"REM", "LOAD", "SAVE", "SHLOAD", "STORE", "RECALL", "PR#", "IN#", "WAIT", "CONT", "LIST", "TRACE",
                    "NOTRACE", "SPEED=", "POP", "NOT", "AND", "OR", "&"}},
  };
  return list;
}

// Every keyword, longest first, so HGR2 is found before HGR and ONERR before
// ON: the browser's list, which is every Applesoft token but the
// one-character operators, plus the categories' own words.
struct Keyword {
  std::string text;
  Kind kind;
};
const std::vector<Keyword> &keywords() {
  static const std::vector<Keyword> list = [] {
    std::map<std::string, Kind> found;
    for (const char *token : a2e::APPLESOFT_TOKENS) {
      const std::string t(token);
      if (t.size() == 1 && t != "&") continue;
      found.emplace(t, Kind::Keyword);
    }
    // A category colours a token; a word in one that is not a token (CLEAR)
    // is not a keyword to Applesoft, so it is not one here.
    for (const Category &c : categories()) {
      for (const char *w : c.words) {
        if (const auto it = found.find(w); it != found.end()) it->second = c.kind;
      }
    }
    std::vector<Keyword> out;
    for (const auto &[text, kind] : found) out.push_back({text, kind});
    std::stable_sort(out.begin(), out.end(), [](const Keyword &a, const Keyword &b) {
      return a.text.size() > b.text.size();
    });
    return out;
  }();
  return list;
}

// The keyword at a position, if one is there, found as Applesoft's tokenizer
// finds it (and the core's, basic_tokenizer.cpp): the longest keyword that
// starts here, whatever follows it. So TOTAL is TO then TAL and SCORE is SC,
// OR, E, which is what the program will do, and the colours say so rather
// than showing a name Applesoft never sees.
const Keyword *keywordAt(std::string_view line, size_t pos) {
  for (const Keyword &k : keywords()) {
    const size_t n = k.text.size();
    if (pos + n > line.size()) continue;
    bool same = true;
    for (size_t i = 0; i < n && same; i++) same = upper(line[pos + i]) == k.text[i];
    if (same) return &k;
  }
  return nullptr;
}

// Where the code of a line starts: after its number and the space after it.
size_t codeStart(std::string_view line) {
  size_t i = 0;
  while (i < line.size() && line[i] == ' ') i++;
  const size_t digits = i;
  while (i < line.size() && isDigit(line[i])) i++;
  if (i == digits) return 0;
  while (i < line.size() && line[i] == ' ') i++;
  return i;
}

bool startsWithWord(std::string_view s, size_t pos, std::string_view word) {
  if (pos + word.size() > s.size()) return false;
  for (size_t i = 0; i < word.size(); i++) {
    if (upper(s[pos + i]) != word[i]) return false;
  }
  return true;
}

// Capitals for what Applesoft would tokenise, and the user's own text where
// Applesoft keeps it: in quotes, after REM, and in a DATA statement.
std::string capitalise(std::string_view code) {
  std::string out;
  out.reserve(code.size());
  bool quote = false, rem = false, data = false;
  for (size_t i = 0; i < code.size(); i++) {
    const char c = code[i];
    if (rem) {
      out += c;
      continue;
    }
    if (c == '"') quote = !quote;
    if (quote || c == '"') {
      out += c;
      continue;
    }
    if (data) {
      if (c == ':') data = false;
      out += c;
      continue;
    }
    if (startsWithWord(code, i, "REM") && (i == 0 || !isAlpha(code[i - 1]))) {
      out += "REM";
      i += 2;
      rem = true;
      continue;
    }
    if (startsWithWord(code, i, "DATA") && (i == 0 || !isAlpha(code[i - 1]))) {
      out += "DATA";
      i += 3;
      data = true;
      continue;
    }
    out += upper(c);
  }
  return out;
}

std::string trim(std::string_view s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
  return std::string(s.substr(a, b - a));
}

std::vector<std::string> splitLines(const std::string &source) {
  std::vector<std::string> lines;
  std::string line;
  for (char c : source) {
    if (c == '\r') continue;
    if (c == '\n') {
      lines.push_back(line);
      line.clear();
    } else {
      line += c;
    }
  }
  lines.push_back(line);
  return lines;
}

// Occurrences of a whole word outside quotes and before any REM.
int countWord(std::string_view code, std::string_view word) {
  int count = 0;
  bool quote = false;
  for (size_t i = 0; i < code.size(); i++) {
    if (code[i] == '"') quote = !quote;
    if (quote) continue;
    if (startsWithWord(code, i, "REM") && (i == 0 || !isAlpha(code[i - 1]))) break;
    if (startsWithWord(code, i, word) && (i == 0 || !isAlpha(code[i - 1])) &&
        (i + word.size() >= code.size() || !isAlpha(code[i + word.size()]))) {
      count++;
    }
  }
  return count;
}

struct Entry {
  int number = -1;
  std::string code;
};

std::vector<Entry> parseEntries(const std::string &source) {
  std::vector<Entry> entries;
  for (const std::string &raw : splitLines(source)) {
    const std::string line = trim(raw);
    if (line.empty()) continue;
    Entry e;
    if (const auto n = lineNumber(line)) {
      e.number = *n;
      e.code = trim(std::string_view(line).substr(codeStart(line)));
    } else {
      e.code = line;
    }
    entries.push_back(std::move(e));
  }
  std::stable_sort(entries.begin(), entries.end(),
                   [](const Entry &a, const Entry &b) { return a.number < b.number; });
  return entries;
}

} // namespace

std::optional<int> lineNumber(std::string_view line) {
  size_t i = 0;
  while (i < line.size() && line[i] == ' ') i++;
  const size_t from = i;
  while (i < line.size() && isDigit(line[i])) i++;
  if (i == from || i - from > 5) return std::nullopt;
  const int n = std::stoi(std::string(line.substr(from, i - from)));
  if (n > 63999) return std::nullopt;
  return n;
}

std::vector<Span> highlight(std::string_view line) {
  std::vector<Span> spans;
  auto add = [&](size_t start, size_t length, Kind kind) {
    if (length == 0) return;
    if (!spans.empty() && spans.back().kind == kind && spans.back().start + spans.back().length == static_cast<int>(start)) {
      spans.back().length += static_cast<int>(length);
      return;
    }
    spans.push_back({static_cast<int>(start), static_cast<int>(length), kind});
  };

  size_t pos = 0;
  while (pos < line.size() && line[pos] == ' ') pos++;
  add(0, pos, Kind::Plain);
  size_t digits = pos;
  while (digits < line.size() && isDigit(line[digits])) digits++;
  add(pos, digits - pos, Kind::LineNumber);
  pos = digits;

  while (pos < line.size()) {
    const char c = line[pos];
    if (c == '"') {
      size_t end = line.find('"', pos + 1);
      end = end == std::string_view::npos ? line.size() : end + 1;
      add(pos, end - pos, Kind::String);
      pos = end;
      continue;
    }
    if (const Keyword *k = keywordAt(line, pos)) {
      add(pos, k->text.size(), k->kind);
      pos += k->text.size();
      if (k->text == "REM") {
        add(pos, line.size() - pos, Kind::Comment);
        break;
      }
      // DATA's items are kept as typed, keywords and all, up to a colon
      // outside quotes: strings and numbers are coloured, nothing else.
      if (k->text == "DATA") {
        bool quote = false;
        while (pos < line.size() && (quote || line[pos] != ':')) {
          const char d = line[pos];
          if (d == '"') quote = !quote;
          add(pos, 1, quote || d == '"' ? Kind::String : isDigit(d) || d == '.' ? Kind::Number
                                                         : d == ','                ? Kind::Punctuation
                                                                                   : Kind::Plain);
          pos++;
        }
      }
      continue;
    }
    if (isDigit(c)) {
      size_t end = pos;
      while (end < line.size() && (isDigit(line[end]) || line[end] == '.')) end++;
      if (end < line.size() && upper(line[end]) == 'E') {
        size_t e = end + 1;
        if (e < line.size() && (line[e] == '+' || line[e] == '-')) e++;
        if (e < line.size() && isDigit(line[e])) {
          while (e < line.size() && isDigit(line[e])) e++;
          end = e;
        }
      }
      add(pos, end - pos, Kind::Number);
      pos = end;
      continue;
    }
    if (isAlpha(c)) {
      // A name ends where a keyword starts inside it, as it does for the
      // tokenizer.
      size_t end = pos + 1;
      while (end < line.size() && isAlnum(line[end]) && !keywordAt(line, end)) end++;
      if (end < line.size() && (line[end] == '$' || line[end] == '%')) end++;
      add(pos, end - pos, Kind::Variable);
      pos = end;
      continue;
    }
    if (std::string_view("+-*/^=<>").find(c) != std::string_view::npos) add(pos, 1, Kind::Operator);
    else if (std::string_view("(),;:").find(c) != std::string_view::npos) add(pos, 1, Kind::Punctuation);
    else add(pos, 1, Kind::Plain);
    pos++;
  }
  return spans;
}

std::vector<Statement> statements(std::string_view line) {
  std::vector<Statement> out;
  const size_t from = codeStart(line);
  if (from == 0 && !lineNumber(line)) return out;
  size_t start = from;
  bool quote = false;
  for (size_t i = from; i < line.size(); i++) {
    const char c = line[i];
    if (c == '"') {
      quote = !quote;
      continue;
    }
    if (quote) continue;
    // After REM the rest of the line is the remark: the core stops counting.
    if (startsWithWord(line, i, "REM")) break;
    if (c == ':') {
      out.push_back({static_cast<int>(start), static_cast<int>(i)});
      start = i + 1;
    }
  }
  out.push_back({static_cast<int>(start), static_cast<int>(line.size())});
  return out;
}

int statementAt(std::string_view line, int column) {
  const std::vector<Statement> list = statements(line);
  for (size_t i = 0; i < list.size(); i++) {
    // A statement owns the colon that ends it.
    if (column >= list[i].start && column <= list[i].end) return static_cast<int>(i);
  }
  return -1;
}

std::vector<int> indentLevels(const std::vector<std::string> &lines) {
  std::vector<int> levels;
  int level = 0;
  for (const std::string &line : lines) {
    const std::string_view code = std::string_view(line).substr(codeStart(line));
    const int fors = countWord(code, "FOR");
    const int nexts = countWord(code, "NEXT");
    if (nexts > fors) level = std::max(0, level - (nexts - fors));
    levels.push_back(level);
    if (fors > nexts) level = std::min(8, level + (fors - nexts));
  }
  return levels;
}

std::string format(const std::string &source) {
  const std::vector<Entry> entries = parseEntries(source);
  size_t width = 0;
  for (const Entry &e : entries) {
    if (e.number >= 0) width = std::max(width, std::to_string(e.number).size());
  }
  std::vector<std::string> codes;
  for (const Entry &e : entries) codes.push_back(e.code);
  const std::vector<int> levels = indentLevels(codes);
  std::string out;
  for (size_t i = 0; i < entries.size(); i++) {
    const Entry &e = entries[i];
    if (!out.empty()) out += '\n';
    if (e.number >= 0) {
      const std::string n = std::to_string(e.number);
      out += std::string(width - n.size(), ' ') + n + ' ';
    }
    out += std::string(static_cast<size_t>(levels[i]) * 2, ' ');
    out += capitalise(e.code);
  }
  return out;
}

Renumbered renumber(const std::string &source, int first, int step) {
  std::vector<Entry> entries;
  for (Entry &e : parseEntries(source)) {
    if (e.number >= 0) entries.push_back(std::move(e));
  }
  Renumbered result;
  int next = first;
  for (const Entry &e : entries) {
    result.mapping[e.number] = next;
    next += step;
  }

  // The targets: a list after GOTO and GOSUB, one number after THEN and
  // RESUME, outside quotes and before any REM.
  auto rewrite = [&](const std::string &code) {
    std::string out;
    bool quote = false;
    size_t i = 0;
    auto copyNumbers = [&](bool list) {
      while (true) {
        while (i < code.size() && code[i] == ' ') out += code[i++];
        const size_t from = i;
        while (i < code.size() && isDigit(code[i])) i++;
        if (i == from) return;
        // A target longer than five digits names no line, and is too long
        // for std::stoi, which throws.
        const auto it = i - from > 5 ? result.mapping.end() : result.mapping.find(std::stoi(code.substr(from, i - from)));
        out += it == result.mapping.end() ? code.substr(from, i - from) : std::to_string(it->second);
        if (!list) return;
        size_t j = i;
        while (j < code.size() && code[j] == ' ') j++;
        if (j >= code.size() || code[j] != ',') return;
        out += code.substr(i, j - i + 1);
        i = j + 1;
      }
    };
    while (i < code.size()) {
      const char c = code[i];
      if (c == '"') quote = !quote;
      if (!quote && startsWithWord(code, i, "REM")) {
        out += code.substr(i);
        break;
      }
      if (!quote) {
        bool matched = false;
        for (const auto &[word, list] : {std::pair<std::string_view, bool>{"GOSUB", true}, {"GOTO", true},
                                         {"THEN", false}, {"RESUME", false}}) {
          if (startsWithWord(code, i, word)) {
            out += code.substr(i, word.size());
            i += word.size();
            copyNumbers(list);
            matched = true;
            break;
          }
        }
        if (matched) continue;
      }
      out += c;
      i++;
    }
    return out;
  };

  std::string text;
  for (const Entry &e : entries) {
    if (!text.empty()) text += '\n';
    text += std::to_string(result.mapping[e.number]) + ' ' + rewrite(e.code);
  }
  result.source = format(text);
  return result;
}

int nextLineNumber(const std::vector<std::string> &lines, int line) {
  for (int i = std::min(line, static_cast<int>(lines.size()) - 1); i >= 0; i--) {
    if (const auto n = lineNumber(lines[static_cast<size_t>(i)])) return std::min(*n + 10, 63999);
  }
  return 10;
}

// ---------------------------------------------------------------------------
// The keyword reference, the browser's KEYWORD_INFO
// ---------------------------------------------------------------------------

namespace {

const KeywordInfo KEYWORD_INFO[] = {
    {"END", "END", "Control", "End program execution"},
    {"FOR", "FOR var = start TO end [STEP n]", "Control", "Start FOR loop"},
    {"NEXT", "NEXT [var]", "Control", "End FOR loop"},
    {"IF", "IF expr THEN statement", "Control", "Conditional execution"},
    {"THEN", "THEN statement", "Control", "Part of IF statement"},
    {"GOTO", "GOTO linenum", "Control", "Jump to line"},
    {"GOSUB", "GOSUB linenum", "Control", "Call subroutine"},
    {"RETURN", "RETURN", "Control", "Return from subroutine"},
    {"ON", "ON expr GOTO/GOSUB line1,line2,...", "Control", "Computed branch"},
    {"STOP", "STOP", "Control", "Stop execution"},
    {"CONT", "CONT", "Control", "Continue after STOP"},
    {"RUN", "RUN [linenum]", "Control", "Run program"},
    {"ONERR", "ONERR GOTO linenum", "Control", "Error handler"},
    {"RESUME", "RESUME", "Control", "Resume after error"},
    {"POP", "POP", "Control", "Pop GOSUB return address"},
    {"PRINT", "PRINT [expr][;,][expr]...", "I/O", "Print to screen"},
    {"INPUT", "INPUT [\"prompt\";]var[,var]...", "I/O", "Get user input"},
    {"GET", "GET var", "I/O", "Get single keypress"},
    {"HOME", "HOME", "I/O", "Clear screen"},
    {"HTAB", "HTAB col", "I/O", "Set horizontal position"},
    {"VTAB", "VTAB row", "I/O", "Set vertical position"},
    {"INVERSE", "INVERSE", "I/O", "Inverse text mode"},
    {"NORMAL", "NORMAL", "I/O", "Normal text mode"},
    {"FLASH", "FLASH", "I/O", "Flashing text mode"},
    {"TEXT", "TEXT", "I/O", "Text mode"},
    {"PR#", "PR# slot", "I/O", "Output to slot"},
    {"IN#", "IN# slot", "I/O", "Input from slot"},
    {"TAB(", "TAB(col)", "I/O", "Tab to column"},
    {"SPC(", "SPC(n)", "I/O", "Print n spaces"},
    {"POS", "POS(0)", "I/O", "Current cursor column"},
    {"GR", "GR", "Lo-Res", "Lo-res graphics mode"},
    {"COLOR=", "COLOR= n", "Lo-Res", "Set lo-res color (0-15)"},
    {"PLOT", "PLOT x,y", "Lo-Res", "Plot lo-res point"},
    {"HLIN", "HLIN x1,x2 AT y", "Lo-Res", "Draw horizontal line"},
    {"VLIN", "VLIN y1,y2 AT x", "Lo-Res", "Draw vertical line"},
    {"SCRN(", "SCRN(x,y)", "Lo-Res", "Get color at point"},
    {"HGR", "HGR", "Hi-Res", "Hi-res page 1"},
    {"HGR2", "HGR2", "Hi-Res", "Hi-res page 2"},
    {"HCOLOR=", "HCOLOR= n", "Hi-Res", "Set hi-res color (0-7)"},
    {"HPLOT", "HPLOT x,y [TO x2,y2]...", "Hi-Res", "Plot/draw hi-res"},
    {"DRAW", "DRAW shape AT x,y", "Hi-Res", "Draw shape"},
    {"XDRAW", "XDRAW shape AT x,y", "Hi-Res", "XOR draw shape"},
    {"ROT=", "ROT= angle", "Hi-Res", "Set shape rotation"},
    {"SCALE=", "SCALE= n", "Hi-Res", "Set shape scale"},
    {"SHLOAD", "SHLOAD", "Hi-Res", "Load shape table"},
    {"LET", "LET var = expr", "Variables", "Assign variable"},
    {"DIM", "DIM var(size)[,var(size)]...", "Variables", "Dimension array"},
    {"DATA", "DATA value,value,...", "Variables", "Define data"},
    {"READ", "READ var[,var]...", "Variables", "Read DATA values"},
    {"RESTORE", "RESTORE", "Variables", "Reset DATA pointer"},
    {"DEF", "DEF FN name(var) = expr", "Variables", "Define function"},
    {"FN", "FN name(expr)", "Variables", "Call user function"},
    {"CLEAR", "CLEAR", "Variables", "Clear variables"},
    {"NEW", "NEW", "Variables", "Clear program"},
    {"ABS", "ABS(n)", "Math", "Absolute value"},
    {"SGN", "SGN(n)", "Math", "Sign (-1,0,1)"},
    {"INT", "INT(n)", "Math", "Integer part"},
    {"SQR", "SQR(n)", "Math", "Square root"},
    {"RND", "RND(n)", "Math", "Random number"},
    {"SIN", "SIN(n)", "Math", "Sine"},
    {"COS", "COS(n)", "Math", "Cosine"},
    {"TAN", "TAN(n)", "Math", "Tangent"},
    {"ATN", "ATN(n)", "Math", "Arctangent"},
    {"LOG", "LOG(n)", "Math", "Natural log"},
    {"EXP", "EXP(n)", "Math", "e^n"},
    {"LEN", "LEN(str$)", "Strings", "String length"},
    {"LEFT$", "LEFT$(str$,n)", "Strings", "Left n chars"},
    {"RIGHT$", "RIGHT$(str$,n)", "Strings", "Right n chars"},
    {"MID$", "MID$(str$,start[,len])", "Strings", "Substring"},
    {"STR$", "STR$(n)", "Strings", "Number to string"},
    {"VAL", "VAL(str$)", "Strings", "String to number"},
    {"ASC", "ASC(str$)", "Strings", "ASCII code"},
    {"CHR$", "CHR$(n)", "Strings", "ASCII to char"},
    {"PEEK", "PEEK(addr)", "System", "Read memory byte"},
    {"POKE", "POKE addr,value", "System", "Write memory byte"},
    {"CALL", "CALL addr", "System", "Call machine code"},
    {"USR", "USR(n)", "System", "Call user routine"},
    {"WAIT", "WAIT addr,mask[,xor]", "System", "Wait for memory"},
    {"HIMEM:", "HIMEM: addr", "System", "Set memory top"},
    {"LOMEM:", "LOMEM: addr", "System", "Set variables start"},
    {"FRE", "FRE(0)", "System", "Free memory"},
    {"PDL", "PDL(n)", "System", "Read paddle (0-255)"},
    {"SPEED=", "SPEED= n", "System", "Set output speed"},
    {"LOAD", "LOAD", "File", "Load from tape"},
    {"SAVE", "SAVE", "File", "Save to tape"},
    {"STORE", "STORE", "File", "Store array"},
    {"RECALL", "RECALL", "File", "Recall array"},
    {"REM", "REM comment", "Other", "Comment"},
    {"LIST", "LIST [start[-end]]", "Other", "List program"},
    {"DEL", "DEL start,end", "Other", "Delete lines"},
    {"TRACE", "TRACE", "Other", "Enable tracing"},
    {"NOTRACE", "NOTRACE", "Other", "Disable tracing"},
    {"&", "& [params]", "Other", "Machine language hook"},
    {"TO", "TO", "Operator", "Range separator"},
    {"STEP", "STEP n", "Operator", "Loop increment"},
    {"AT", "AT", "Operator", "Position specifier"},
    {"AND", "expr AND expr", "Operator", "Logical AND"},
    {"OR", "expr OR expr", "Operator", "Logical OR"},
    {"NOT", "NOT expr", "Operator", "Logical NOT"},
};

// What a program defines, for completion: the browser's ProgramContext.
struct ProgramContext {
  std::set<std::string> numbers;
  std::set<std::string> variables; // with $ or % as written
  std::set<std::string> arrays;
  std::map<std::string, std::string> functions; // name to parameter
};

bool isToken(const std::string &name) {
  for (const char *t : a2e::APPLESOFT_TOKENS) {
    if (name == t) return true;
  }
  return false;
}

ProgramContext readProgram(const std::string &source) {
  ProgramContext p;
  static const std::regex defFn(R"(DEF\s*FN\s*([A-Z][A-Z0-9]*)\s*\(\s*([A-Z][A-Z0-9]*)\s*\))");
  static const std::regex dim(R"(DIM\s+(.+))");
  static const std::regex forVar(R"(FOR\s*([A-Z][A-Z0-9]*)\s*=)");
  static const std::regex list(R"((INPUT|READ|GET)\s+(?:"[^"]*"\s*;\s*)?(.+))");
  static const std::regex assign(R"(^\s*(?:LET\s+)?([A-Z][A-Z0-9]*[$%]?)\s*=)");
  static const std::regex name(R"(([A-Z][A-Z0-9]*[$%]?)(\()?)");
  auto addName = [&](const std::string &n, bool array) {
    if (n.empty() || isToken(n)) return;
    (array ? p.arrays : p.variables).insert(n);
  };
  for (const std::string &line : splitLines(source)) {
    if (const auto n = lineNumber(line)) p.numbers.insert(std::to_string(*n));
    const std::string code = uppercase(std::string_view(line).substr(codeStart(line)));
    const std::string &upperLine = code;
    // A statement at a time: they are what the patterns describe.
    std::vector<std::string> parts;
    {
      std::string part;
      bool quote = false;
      for (char c : upperLine) {
        if (c == '"') quote = !quote;
        if (c == ':' && !quote) {
          parts.push_back(part);
          part.clear();
        } else {
          part += c;
        }
      }
      parts.push_back(part);
    }
    for (const std::string &part : parts) {
      std::smatch m;
      if (std::regex_search(part, m, defFn)) {
        p.functions[m[1]] = m[2];
        addName(m[2], false);
      }
      if (std::regex_search(part, m, dim)) {
        const std::string rest = m[1];
        for (auto it = std::sregex_iterator(rest.begin(), rest.end(), name); it != std::sregex_iterator(); ++it) {
          if ((*it)[2].matched) addName((*it)[1], true);
        }
      }
      if (std::regex_search(part, m, forVar)) addName(m[1], false);
      if (std::regex_search(part, m, list)) {
        const std::string rest = m[2];
        for (auto it = std::sregex_iterator(rest.begin(), rest.end(), name); it != std::sregex_iterator(); ++it) {
          addName((*it)[1], (*it)[2].matched);
        }
      }
      if (part.find("FOR") == std::string::npos && std::regex_search(part, m, assign)) addName(m[1], false);
    }
  }
  return p;
}

bool hasPrefix(const std::string &s, std::string_view prefix) {
  if (s.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); i++) {
    if (upper(s[i]) != upper(prefix[i])) return false;
  }
  return true;
}

} // namespace

const KeywordInfo *keywordInfo(std::string_view keyword) {
  const std::string k = uppercase(keyword);
  for (const KeywordInfo &info : KEYWORD_INFO) {
    if (k == info.keyword) return &info;
  }
  return nullptr;
}

std::string_view wordBefore(std::string_view before) {
  size_t i = before.size();
  while (i > 0 && (isAlnum(before[i - 1]) || before[i - 1] == '$')) i--;
  return before.substr(i);
}

std::vector<Completion> complete(const std::string &source, std::string_view before, size_t limit) {
  std::vector<Completion> out;
  const std::string_view word = wordBefore(before);
  if (word.size() < 2) return out;
  const std::string_view lead = before.substr(0, before.size() - word.size());
  // Not inside a string.
  if (std::count(lead.begin(), lead.end(), '"') % 2) return out;
  const std::string leadUpper = uppercase(lead);

  static const std::regex afterJump(R"((GOTO|GOSUB|THEN)\s*$)");
  static const std::regex afterOn(R"(ON\s.*(GOTO|GOSUB)\s+[\d,\s]*$)");
  static const std::regex afterOnErr(R"(ONERR\s+GOTO\s*$)");
  static const std::regex afterFn(R"(FN\s*$)");
  const bool lines = std::regex_search(leadUpper, afterJump) || std::regex_search(leadUpper, afterOn) ||
                     std::regex_search(leadUpper, afterOnErr);
  const bool functions = std::regex_search(leadUpper, afterFn);
  const ProgramContext p = readProgram(source);

  auto push = [&](Completion c) {
    if (out.size() < limit) out.push_back(std::move(c));
  };
  if (lines) {
    std::vector<int> numbers;
    for (const std::string &n : p.numbers) {
      if (hasPrefix(n, word)) numbers.push_back(std::stoi(n));
    }
    std::sort(numbers.begin(), numbers.end());
    for (int n : numbers) {
      push({Completion::Type::LineNumber, std::to_string(n), "Line", "Line " + std::to_string(n), "Program line"});
    }
    return out;
  }
  for (const auto &[name, param] : p.functions) {
    if (hasPrefix(name, word)) {
      push({Completion::Type::Function, name, "Function", "FN " + name + "(" + param + ")", "User-defined function"});
    }
  }
  if (functions) return out;
  std::vector<Completion> vars;
  for (const std::string &v : p.variables) {
    if (hasPrefix(v, word)) {
      vars.push_back({Completion::Type::Variable, v, v.back() == '$' ? "String Var" : "Variable", v, "Variable"});
    }
  }
  for (const std::string &a : p.arrays) {
    if (hasPrefix(a, word)) {
      vars.push_back({Completion::Type::Variable, a, a.back() == '$' ? "String Var" : "Variable", a + "(...)",
                      "Array variable"});
    }
  }
  std::sort(vars.begin(), vars.end(), [](const Completion &x, const Completion &y) { return x.text < y.text; });
  // Functions were offered first above; variables come before them in the
  // browser, so put them in front.
  out.insert(out.begin(), vars.begin(), vars.end());
  if (out.size() > limit) out.resize(limit);
  std::vector<Completion> words;
  std::set<std::string> seen;
  for (const char *token : a2e::APPLESOFT_TOKENS) {
    const std::string t(token);
    if (t.size() < 2 || !hasPrefix(t, word) || !seen.insert(t).second) continue;
    const KeywordInfo *info = keywordInfo(t);
    words.push_back({Completion::Type::Keyword, t, info ? info->category : "Other", info ? info->syntax : t,
                     info ? info->description : ""});
  }
  std::sort(words.begin(), words.end(), [](const Completion &x, const Completion &y) { return x.text < y.text; });
  for (Completion &c : words) push(std::move(c));
  return out;
}

RunChoice chooseRun(const RunFacts &f) {
  // Nothing to choose between: no program in the editor, or the same one in
  // both places.
  if (f.editorEmpty || f.memoryMatchesEditor) return RunChoice::RunMemory;
  // Nothing in memory to lose.
  if (f.memoryEmpty) return RunChoice::WriteThenRun;
  if (f.synced) {
    // Read or written, and not edited since: memory is the program, even if
    // something has since LOADed another into it.
    if (!f.editedSinceSync) return RunChoice::RunMemory;
    // Edited, and memory still holds what the editor last agreed with: the
    // edits are what is meant.
    if (!f.memoryChangedSinceSync) return RunChoice::WriteThenRun;
  }
  // Two different programs and nothing to say which: a text restored at
  // launch or opened from a file against one in memory, or edits against a
  // program that has changed under them.
  return RunChoice::Ask;
}

std::string controlText(unsigned char c) {
  return a2e::basic_text::needsToken(c) ? a2e::basic_text::tokenFor(c) : std::string();
}

std::string errorMessage(uint8_t code) {
  switch (code) {
  case 0x00: return "NEXT WITHOUT FOR";
  case 0x10: return "SYNTAX ERROR";
  case 0x16: return "RETURN WITHOUT GOSUB";
  case 0x2A: return "OUT OF DATA";
  case 0x35: return "ILLEGAL QUANTITY";
  case 0x45: return "OVERFLOW";
  case 0x4D: return "OUT OF MEMORY";
  case 0x5A: return "UNDEF'D STATEMENT";
  case 0x6B: return "BAD SUBSCRIPT";
  case 0x78: return "REDIM'D ARRAY";
  case 0x85: return "DIVISION BY ZERO";
  case 0x95: return "ILLEGAL DIRECT";
  case 0xA3: return "TYPE MISMATCH";
  case 0xB0: return "STRING TOO LONG";
  case 0xBF: return "FORMULA TOO COMPLEX";
  case 0xD2: return "CAN'T CONTINUE";
  case 0xE0: return "UNDEF'D FUNCTION";
  }
  char text[16];
  std::snprintf(text, sizeof text, "ERROR %u", code);
  return text;
}

std::string formatReal(double value) {
  char text[48];
  if (value == 0) return "0";
  if (std::floor(value) == value && std::fabs(value) < 1e15) {
    std::snprintf(text, sizeof text, "%.0f", value);
    return text;
  }
  const double magnitude = std::fabs(value);
  if (magnitude >= 0.01 && magnitude < 1e7) {
    std::snprintf(text, sizeof text, "%.9g", value);
    std::string s = text;
    if (s.find('.') != std::string::npos && s.find('e') == std::string::npos) {
      while (!s.empty() && s.back() == '0') s.pop_back();
      if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
  }
  std::snprintf(text, sizeof text, "%.6e", value);
  // 1.234560e+07 as JavaScript writes it: 1.234560e+7.
  std::string s = text;
  const size_t e = s.find('e');
  if (e != std::string::npos && e + 2 < s.size() && s[e + 2] == '0' && e + 3 < s.size()) s.erase(e + 2, 1);
  return s;
}

} // namespace a2e::native::basic
