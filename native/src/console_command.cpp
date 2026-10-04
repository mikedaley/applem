/*
 * console_command.cpp - What a line typed into the debugging console asks for
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "console_command.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <optional>

namespace a2e::native {

namespace {

using Kind = ConsoleCommand::Kind;

const std::vector<ConsoleCommandHelp> COMMANDS = {
    {"help", "h", "help [command]", "List the commands, or explain one"},
    {"cls", "clear", "cls", "Clear the console"},
    {"m", "mem dump", "m <from>[.<to>]", "Show memory, a line of it or a range"},
    {"w", "write", "w <address> <byte>...", "Write bytes into memory"},
    {"f", "fill", "f <from>.<to> <byte>", "Fill a range of memory with one byte"},
    {"l", "list u", "l [address] [lines]", "Disassemble, from the PC if no address is given"},
    {"g", "go c continue", "g [address]", "Run, from an address if one is given"},
    {"s", "step t", "s [count]", "Step one instruction, or several"},
    {"n", "next over", "n", "Step over a subroutine call"},
    {"finish", "out", "finish", "Run until the current subroutine returns"},
    {"pause", "break", "pause", "Stop the machine"},
    {"until", "to", "until <address>", "Run until the PC reaches an address"},
    {"r", "reg regs registers", "r [a=<value> pc=<address> ...]", "Show the registers, or set them"},
    {"?", "print p eval", "? <expression>", "Print an expression's value: PEEK($24)+1, A, X*2"},
    {"bp", "break b", "bp [r|w|rw|sp] <address>[-<end>] [if <condition>]",
     "Break on execution, a read, a write, an access or the stack pointer"},
    {"bp sw", "", "bp sw <switch> [on|off|=<value>|&<mask>=<value>] [if <condition>]",
     "Break when a soft switch changes, or comes to a value"},
    {"bp beam", "", "bp beam vbl|hbl|line <n>|col <n>|<line>,<col> [if <condition>]",
     "Break when the beam reaches a line, a column, or blanking"},
    {"bl", "breaks", "bl", "List the breakpoints, numbered"},
    {"bd", "delete del", "bd <n>|all", "Delete a breakpoint"},
    {"be", "enable", "be <n>|all", "Enable a breakpoint"},
    {"bx", "disable", "bx <n>|all", "Disable a breakpoint"},
    {"sym", "symbol", "sym <name>|<address>", "Look up a name's address, or an address's name"},
    {"stack", "st", "stack", "Show the stack"},
    {"trace", "tr", "trace [count]", "Show the last instructions run"},
    {"find", "search", "find <byte>... | find \"text\"", "Search memory; ?? matches any byte"},
    {"reset", "", "reset", "Press Control-Reset"},
    {"reboot", "", "reboot", "Turn the machine off and on"},
};

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

bool isHex(const std::string &s) {
  return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); });
}

// The words of a line, a quoted string kept whole with its quotes.
std::vector<std::string> words(const std::string &line) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
    if (i >= line.size()) break;
    size_t j = i;
    if (line[i] == '"') {
      j = line.find('"', i + 1);
      j = j == std::string::npos ? line.size() : j + 1;
    } else {
      while (j < line.size() && !std::isspace(static_cast<unsigned char>(line[j]))) j++;
    }
    out.push_back(line.substr(i, j - i));
    i = j;
  }
  return out;
}

// Everything after the first `n` words, as typed.
std::string rest(const std::string &line, size_t n) {
  size_t i = 0;
  for (size_t w = 0; w < n; w++) {
    while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
    if (i < line.size() && line[i] == '"') {
      const size_t close = line.find('"', i + 1);
      i = close == std::string::npos ? line.size() : close + 1;
    } else {
      while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i]))) i++;
    }
  }
  while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
  std::string out = line.substr(i);
  while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back()))) out.pop_back();
  return out;
}

ConsoleCommand error(const std::string &why) {
  ConsoleCommand c;
  c.kind = Kind::Error;
  c.error = why;
  return c;
}

// The command a word names, by its name or any alias.
const ConsoleCommandHelp *commandFor(const std::string &word) {
  const std::string w = lower(word);
  for (const ConsoleCommandHelp &c : COMMANDS) {
    if (w == c.name) return &c;
    const std::vector<std::string> aliases = words(c.aliases);
    if (std::find(aliases.begin(), aliases.end(), w) != aliases.end()) return &c;
  }
  return nullptr;
}

// ---- The monitor's own syntax ----

// A monitor address at `at`: hex, or a bank, a slash and hex on a IIgs.
// Returns how many characters it took, and the address as the symbol
// resolver reads a number ("$300", "E1/2000"), or 0.
size_t monitorAddress(const std::string &line, size_t at, std::string &out) {
  // A dollar sign is allowed, and needed for an address that spells a
  // command: $BE is memory, be is enable.
  if (at < line.size() && line[at] == '$') {
    const size_t taken = monitorAddress(line, at + 1, out);
    return taken ? taken + 1 : 0;
  }
  size_t i = at;
  while (i < line.size() && std::isxdigit(static_cast<unsigned char>(line[i]))) i++;
  if (i == at || i - at > 4) {
    if (i - at <= 2 && i < line.size() && line[i] == '/') {
      // falls through to the bank form below
    } else {
      return 0;
    }
  }
  if (i < line.size() && line[i] == '/') {
    if (i - at > 2) return 0;
    const size_t bankEnd = i;
    size_t j = i + 1;
    while (j < line.size() && std::isxdigit(static_cast<unsigned char>(line[j]))) j++;
    if (j == i + 1 || j - i - 1 > 4) return 0;
    out = line.substr(at, bankEnd - at) + "/" + line.substr(i + 1, j - i - 1);
    return j - at;
  }
  out = "$" + line.substr(at, i - at);
  return i - at;
}

// 300, 300.3FF, 300: A9 00, 300L, 300G, E1/2000.20FF. Empty if the line is
// not in that form.
std::optional<ConsoleCommand> parseMonitor(const std::string &line) {
  size_t i = 0;
  while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i]))) i++;
  std::string from;
  const size_t taken = monitorAddress(line, i, from);
  if (!taken) return std::nullopt;
  i += taken;
  ConsoleCommand c;
  c.from = from;

  auto restFrom = [&](size_t at) {
    std::string r = line.substr(at);
    while (!r.empty() && std::isspace(static_cast<unsigned char>(r.front()))) r.erase(r.begin());
    while (!r.empty() && std::isspace(static_cast<unsigned char>(r.back()))) r.pop_back();
    return r;
  };

  if (i >= line.size() || restFrom(i).empty()) {
    c.kind = Kind::Dump;
    return c;
  }
  const char next = line[i];
  if (next == '.') {
    std::string to;
    const size_t t = monitorAddress(line, i + 1, to);
    if (!t) return error("A range ends in an address: 300.3FF");
    c.kind = Kind::Dump;
    c.to = to;
    const std::string after = restFrom(i + 1 + t);
    if (!after.empty()) return error("Nothing goes after a range");
    return c;
  }
  if (next == ':') {
    c.kind = Kind::Write;
    for (const std::string &w : words(line.substr(i + 1))) {
      if (!isHex(w) || w.size() > 2) return error("The monitor writes bytes in hex: 300: A9 00 8D");
      c.values.push_back("$" + w);
    }
    if (c.values.empty()) return error("Nothing to write: 300: A9 00 8D");
    return c;
  }
  const std::string word = lower(restFrom(i));
  if (word == "l") {
    c.kind = Kind::List;
    return c;
  }
  // 300L 40: as many lines as asked, which the monitor itself does not take.
  if (word.size() > 1 && word[0] == 'l' && std::isspace(static_cast<unsigned char>(word[1]))) {
    const std::string count = restFrom(i + 1);
    char *end = nullptr;
    const long n = std::strtol(count.c_str(), &end, 10);
    if (*end || n < 1) return error("List as many lines as a number says: 300L 40");
    c.kind = Kind::List;
    c.count = static_cast<int>(n);
    return c;
  }
  if (word == "g") {
    c.kind = Kind::Go;
    return c;
  }
  return std::nullopt;
}

// ---- Breakpoints ----

// A range as the debugger's own words write it: "COUT", "$2000-$20FF".
void splitRange(const std::string &text, std::string &from, std::string &to) {
  const size_t dash = text.find('-', 1);
  if (dash == std::string::npos) {
    from = text;
    to.clear();
  } else {
    from = text.substr(0, dash);
    to = text.substr(dash + 1);
  }
}

// What follows "if", if anything.
std::string conditionAfter(const std::vector<std::string> &w, size_t &end) {
  for (size_t i = 0; i < w.size(); i++) {
    if (lower(w[i]) == "if") {
      end = i;
      std::string cond;
      for (size_t j = i + 1; j < w.size(); j++) cond += (cond.empty() ? "" : " ") + w[j];
      return cond;
    }
  }
  end = w.size();
  return {};
}

std::optional<int> number(const std::string &text) {
  if (text.empty()) return std::nullopt;
  char *end = nullptr;
  const long v = std::strtol(text.c_str(), &end, 10);
  if (*end) return std::nullopt;
  return static_cast<int>(v);
}

// A byte as a breakpoint on a register is written: $80, 80, 0x80.
std::optional<uint8_t> byteValue(std::string text) {
  if (!text.empty() && text[0] == '$') text.erase(0, 1);
  else if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) text.erase(0, 2);
  if (!isHex(text) || text.size() > 2) return std::nullopt;
  return static_cast<uint8_t>(std::strtoul(text.c_str(), nullptr, 16));
}

ConsoleCommand parseBreakpoint(const std::vector<std::string> &all) {
  // all[0] is "bp"; the condition, if any, is cut off first.
  size_t end = 0;
  ConsoleCommand c;
  c.kind = Kind::BreakAdd;
  c.breakpoint.condition = conditionAfter(all, end);
  const std::vector<std::string> w(all.begin() + 1, all.begin() + static_cast<long>(end));
  if (end < all.size() && c.breakpoint.condition.empty()) return error("A condition follows \"if\": bp $2000 if A == $41");
  if (w.empty()) return error("Break where? bp $2000, bp w $0400-$07FF, bp sw page2, bp beam vbl");

  const std::string head = lower(w[0]);

  if (head == "sw" || head == "switch") {
    if (w.size() < 2) return error("Which switch? bp sw page2 on");
    Breakpoint &b = c.breakpoint;
    b.kind = Breakpoint::Kind::Switch;
    std::string spec = w[1];
    for (size_t i = 2; i < w.size(); i++) spec += " " + w[i];
    // "page2", "page2 on", "page2 off", "newvideo=$C1", "newvideo&$80=$80",
    // "newvideo & $80 = $80": the spaces are only for reading.
    std::string compact;
    for (char ch : spec) {
      if (ch != ' ') compact += ch;
    }
    std::string key = compact;
    const size_t amp = compact.find('&');
    const size_t eq = compact.find('=');
    const size_t cut = std::min(amp, eq);
    if (cut != std::string::npos) {
      key = compact.substr(0, cut);
      if (eq == std::string::npos) return error("A mask needs a value: bp sw newvideo&$80=$80");
      const auto value = byteValue(compact.substr(eq + 1));
      if (!value) return error("A switch's value is a byte: bp sw newvideo=$C1");
      uint8_t mask = 0xFF;
      if (amp != std::string::npos && amp < eq) {
        const auto m = byteValue(compact.substr(amp + 1, eq - amp - 1));
        if (!m || !*m) return error("A mask is a byte, not zero: bp sw newvideo&$80=$80");
        mask = *m;
      }
      b.equals = true;
      b.mask = mask;
      b.value = *value & mask;
    } else if (w.size() >= 3) {
      key = w[1];
      const std::string state = lower(w[2]);
      if (state == "on" || state == "off") {
        b.equals = true;
        b.value = state == "on" ? 1 : 0;
      } else if (state != "change" && state != "changes") {
        return error("A switch breaks on a change, on or off, or a value: bp sw page2 on");
      }
      if (w.size() > 3) return error("Too much after the switch");
    }
    b.key = lower(key);
    if (b.key.empty()) return error("Which switch? bp sw page2 on");
    return c;
  }

  if (head == "beam") {
    if (w.size() < 2) return error("Where? bp beam vbl, bp beam line 100, bp beam 100,20");
    Breakpoint &b = c.breakpoint;
    b.kind = Breakpoint::Kind::Beam;
    const std::string what = lower(w[1]);
    // The column is in visible columns; whoever runs it knows the machine's
    // blanking and turns it into the scanner's position.
    if (what == "vbl") {
      b.beamMode = Breakpoint::BeamVbl;
    } else if (what == "hbl") {
      b.beamMode = Breakpoint::BeamHbl;
    } else if (what == "line" || what == "col" || what == "column") {
      if (w.size() < 3) return error("Which one? bp beam line 100");
      const auto n = number(w[2]);
      if (!n || *n < 0) return error("A line or a column is a number");
      b.beamMode = what == "line" ? Breakpoint::BeamLine : Breakpoint::BeamColumn;
      (what == "line" ? b.scanline : b.hPos) = *n;
    } else if (const size_t comma = what.find(','); comma != std::string::npos) {
      const auto line = number(what.substr(0, comma));
      const auto col = number(what.substr(comma + 1));
      if (!line || !col || *line < 0 || *col < 0) return error("A line and a column: bp beam 100,20");
      b.beamMode = Breakpoint::BeamLineColumn;
      b.scanline = *line;
      b.hPos = *col;
    } else {
      return error("bp beam vbl, hbl, line <n>, col <n> or <line>,<col>");
    }
    return c;
  }

  // An address, a range or a name, after a kind if one is given.
  Breakpoint &b = c.breakpoint;
  size_t at = 0;
  if (head == "r" || head == "read") {
    b.kind = Breakpoint::Kind::Read;
    at = 1;
  } else if (head == "w" || head == "write") {
    b.kind = Breakpoint::Kind::Write;
    at = 1;
  } else if (head == "rw" || head == "access") {
    b.kind = Breakpoint::Kind::ReadWrite;
    at = 1;
  } else if (head == "sp" || head == "stack") {
    b.kind = Breakpoint::Kind::Stack;
    at = 1;
  } else if (head == "x" || head == "exec") {
    at = 1;
  }
  if (at >= w.size()) return error("Break where? bp w $0400-$07FF");
  if (w.size() > at + 1) return error("One address or range: bp $2000-$20FF (a condition follows \"if\")");
  splitRange(w[at], c.from, c.to);
  if (c.from.empty()) return error("Break where? bp $2000");
  return c;
}

// "3", "all", for bd/be/bx.
ConsoleCommand parseIndex(Kind kind, const std::vector<std::string> &w) {
  ConsoleCommand c;
  c.kind = kind;
  if (w.size() < 2) return error(std::string("Which one? ") + lower(w[0]) + " 1, or " + lower(w[0]) + " all");
  if (lower(w[1]) == "all") {
    c.all = true;
    return c;
  }
  const auto n = number(w[1]);
  if (!n || *n < 1) return error("A breakpoint is numbered from 1, as bl lists them");
  c.count = *n;
  return c;
}

} // namespace

const std::vector<ConsoleCommandHelp> &consoleCommands() { return COMMANDS; }

ConsoleCommand parseConsoleCommand(const std::string &line) {
  const std::vector<std::string> w = words(line);
  if (w.empty()) return {};

  const ConsoleCommandHelp *command = commandFor(w[0]);
  // A word that is not a command may be the monitor's: 300.3FF, 300L.
  if (!command) {
    if (auto monitor = parseMonitor(line)) return *monitor;
    return error("No command \"" + w[0] + "\"; help lists them");
  }
  const std::string name = command->name;
  ConsoleCommand c;
  auto optionalCount = [&](size_t at) -> bool {
    if (w.size() <= at) return true;
    const auto n = number(w[at]);
    if (!n || *n < 1) return false;
    c.count = *n;
    return w.size() == at + 1;
  };

  if (name == "help") {
    c.kind = Kind::Help;
    c.text = rest(line, 1);
  } else if (name == "cls") {
    c.kind = Kind::Clear;
  } else if (name == "m") {
    if (w.size() < 2) return error("Show what? m $2000, m $2000.$20FF");
    c.kind = Kind::Dump;
    // "m 300.3FF" and "m $300 $3FF" both.
    const std::string arg = w[1];
    const size_t dot = arg.find('.', 1);
    if (dot != std::string::npos) {
      c.from = arg.substr(0, dot);
      c.to = arg.substr(dot + 1);
    } else {
      c.from = arg;
      if (w.size() > 2) c.to = w[2];
    }
    if (w.size() > (dot != std::string::npos ? 2u : 3u)) return error("m <from>[.<to>]");
  } else if (name == "w") {
    if (w.size() < 3) return error("Write what where? w $300 A9 00 8D");
    c.kind = Kind::Write;
    c.from = w[1];
    c.values.assign(w.begin() + 2, w.end());
  } else if (name == "f") {
    if (w.size() != 3) return error("f <from>.<to> <byte>");
    const size_t dot = w[1].find('.', 1);
    if (dot == std::string::npos) return error("Fill a range: f $2000.$3FFF 00");
    c.kind = Kind::Fill;
    c.from = w[1].substr(0, dot);
    c.to = w[1].substr(dot + 1);
    c.values.push_back(w[2]);
  } else if (name == "l") {
    c.kind = Kind::List;
    if (w.size() > 1) c.from = w[1];
    if (w.size() > 2) {
      const auto n = number(w[2]);
      if (!n || *n < 1) return error("l [address] [lines]");
      c.count = *n;
    }
    if (w.size() > 3) return error("l [address] [lines]");
  } else if (name == "g") {
    c.kind = Kind::Go;
    if (w.size() > 1) c.from = w[1];
    if (w.size() > 2) return error("g [address]");
  } else if (name == "s") {
    c.kind = Kind::Step;
    if (!optionalCount(1)) return error("s [count]");
  } else if (name == "n") {
    c.kind = Kind::StepOver;
  } else if (name == "finish") {
    c.kind = Kind::StepOut;
  } else if (name == "pause") {
    c.kind = Kind::Pause;
  } else if (name == "until") {
    if (w.size() != 2) return error("until <address>");
    c.kind = Kind::Until;
    c.from = w[1];
  } else if (name == "r") {
    c.kind = Kind::Registers;
    // "a=42 pc=$2000", or "a = 42": the spaces are only for reading.
    std::string compact;
    for (char ch : rest(line, 1)) {
      if (ch != ' ') compact += ch;
      else if (!compact.empty() && compact.back() != ',') compact += ',';
    }
    // Every name=value, split at the commas the spaces became.
    std::vector<std::string> parts;
    std::string part;
    for (char ch : compact) {
      if (ch == ',') {
        if (!part.empty()) parts.push_back(part);
        part.clear();
      } else {
        part += ch;
      }
    }
    if (!part.empty()) parts.push_back(part);
    // A part may be "a", "=", "42" after the spaces split them; join those.
    std::vector<std::string> joined;
    for (const std::string &p : parts) {
      if (!joined.empty() && (p.front() == '=' || joined.back().back() == '=')) joined.back() += p;
      else joined.push_back(p);
    }
    static const char *const REGISTERS[] = {"a", "x", "y", "sp", "s", "pc", "p", "pbr", "k", "dbr", "b", "d", "dp"};
    for (const std::string &p : joined) {
      const size_t eq = p.find('=');
      if (eq == std::string::npos || eq == 0 || eq + 1 >= p.size()) return error("Set a register as r a=$42 pc=COUT");
      const std::string reg = lower(p.substr(0, eq));
      if (std::find_if(std::begin(REGISTERS), std::end(REGISTERS), [&](const char *r) { return reg == r; }) ==
          std::end(REGISTERS)) {
        return error("No register \"" + p.substr(0, eq) + "\": a x y sp pc p, and pbr dbr d on a 65816");
      }
      c.assignments.emplace_back(reg, p.substr(eq + 1));
    }
  } else if (name == "?") {
    c.kind = Kind::Evaluate;
    c.text = rest(line, 1);
    if (c.text.empty()) return error("Print what? ? PEEK($24) + 1");
  } else if (name == "bp") {
    return parseBreakpoint(w);
  } else if (name == "bl") {
    c.kind = Kind::BreakList;
  } else if (name == "bd") {
    return parseIndex(Kind::BreakDelete, w);
  } else if (name == "be") {
    return parseIndex(Kind::BreakEnable, w);
  } else if (name == "bx") {
    return parseIndex(Kind::BreakDisable, w);
  } else if (name == "sym") {
    if (w.size() != 2) return error("sym <name> or sym <address>");
    c.kind = Kind::Symbol;
    c.text = w[1];
  } else if (name == "stack") {
    c.kind = Kind::Stack;
  } else if (name == "trace") {
    c.kind = Kind::Trace;
    if (!optionalCount(1)) return error("trace [count]");
  } else if (name == "find") {
    if (w.size() < 2) return error("Find what? find A9 ?? 8D, or find \"HELLO\"");
    c.kind = Kind::Find;
    if (w[1].front() == '"') {
      const std::string text = rest(line, 1);
      if (text.size() < 3 || text.back() != '"') return error("A string is in quotes: find \"HELLO\"");
      c.text = text.substr(1, text.size() - 2);
    } else {
      for (size_t i = 1; i < w.size(); i++) {
        std::string byte = w[i];
        if (byte == "??") {
          c.values.push_back(byte);
          continue;
        }
        if (!byteValue(byte)) return error("Bytes in hex, ?? for any: find A9 ?? 8D");
        if (byte[0] != '$') byte = "$" + byte;
        c.values.push_back(byte);
      }
    }
  } else if (name == "reset") {
    c.kind = Kind::Reset;
  } else if (name == "reboot") {
    c.kind = Kind::Reboot;
  }
  return c;
}

} // namespace a2e::native
