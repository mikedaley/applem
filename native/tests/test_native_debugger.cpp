/*
 * test_native_debugger.cpp - The debugger's names for addresses and its breakpoint list
 *
 * What the user types into the debugger is read here, so the rules are
 * pinned: "$" always means a number, a name wins over a bare word that
 * happens to be hex, a bank and a slash is a IIgs address, and nothing wider
 * than the machine's bus is accepted. The breakpoint list must hand the core
 * exactly the enabled entries, take back what it handed over, and come back
 * from the settings as it went in, every kind of it: addresses, the stack,
 * soft switches and the beam, which every window shares.
 */

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include <cstring>

#include "../src/debug_breakpoints.hpp"
#include "../src/debug_symbols.hpp"
#include "debug/machine_debug.hpp"
#include "debug/soft_switch_catalog.hpp"
#include "machine/machine_profile.hpp"

using namespace a2e;
using namespace a2e::native;

TEST_CASE("The Apple II's own names come from symbols.js", "[debugger][symbols]") {
  DebugSymbols symbols;
  const auto kbd = symbols.lookup(0xC000);
  REQUIRE(kbd);
  REQUIRE(kbd->name == "KBD");
  REQUIRE(kbd->category == DebugSymbols::Category::SoftSwitch);
  REQUIRE(symbols.lookup(0x0024)->name == "CH");
  // The built-in names are the 8-bit machines'; a banked address has none.
  REQUIRE_FALSE(symbols.lookup(0xE1C000));
  REQUIRE_FALSE(symbols.lookup(0x0002));
}

TEST_CASE("An address is read the way the user wrote it", "[debugger][symbols]") {
  DebugSymbols symbols;
  REQUIRE(symbols.resolve("$2000", 0xFFFF) == 0x2000u);
  REQUIRE(symbols.resolve("2000", 0xFFFF) == 0x2000u);
  REQUIRE(symbols.resolve("0x2000", 0xFFFF) == 0x2000u);
  REQUIRE(symbols.resolve("  $fded ", 0xFFFF) == 0xFDEDu);
  REQUIRE(symbols.resolve("kbd", 0xFFFF) == 0xC000u);
  REQUIRE(symbols.resolve("E1/2000", 0xFFFFFF) == 0xE12000u);
  REQUIRE(symbols.resolve("$E1/2000", 0xFFFFFF) == 0xE12000u);
  // Nothing wider than the machine's bus.
  REQUIRE_FALSE(symbols.resolve("E1/2000", 0xFFFF));
  REQUIRE_FALSE(symbols.resolve("$12345", 0xFFFF));
  REQUIRE_FALSE(symbols.resolve("NOSUCHNAME", 0xFFFF));
  REQUIRE_FALSE(symbols.resolve("", 0xFFFF));

  // A word that is also hex is a name once something has that name, and a
  // "$" makes it a number again.
  REQUIRE(symbols.resolve("BEEF", 0xFFFF) == 0xBEEFu);
  symbols.setLabel(0x0300, "BEEF");
  REQUIRE(symbols.resolve("BEEF", 0xFFFF) == 0x0300u);
  REQUIRE(symbols.resolve("$BEEF", 0xFFFF) == 0xBEEFu);
}

TEST_CASE("The user's labels come first and go when emptied", "[debugger][symbols]") {
  DebugSymbols symbols;
  symbols.setLabel(0xC000, "KEYS");
  REQUIRE(symbols.lookup(0xC000)->name == "KEYS");
  REQUIRE(symbols.lookup(0xC000)->category == DebugSymbols::Category::User);
  symbols.setComment(0xC000, "the keyboard");
  REQUIRE(symbols.label(0xC000)->comment == "the keyboard");
  symbols.setLabel(0xC000, "");
  REQUIRE(symbols.lookup(0xC000)->name == "KBD"); // the comment keeps the entry...
  symbols.setComment(0xC000, "");
  REQUIRE(symbols.label(0xC000) == nullptr); // ...until it is gone too
}

TEST_CASE("Each assembler's symbol file is understood", "[debugger][symbols]") {
  DebugSymbols symbols;
  const int count = symbols.importSymbols(
      "; a comment\n"
      "* another\n"
      "sym\tid=0,name=\"START\",addrsize=absolute,val=0x2000,type=lab\n"
      "PLAYER EQU $0300\n"
      "score = $0310\n"
      "!addr SPEED = $0320\n"
      "$4000 TABLE\n"
      "5000 ABCD\n"); // a hex word as the name is not a symbol
  REQUIRE(count == 5);
  REQUIRE(symbols.lookup(0x2000)->name == "START");
  REQUIRE(symbols.lookup(0x0300)->name == "PLAYER");
  REQUIRE(symbols.lookup(0x0310)->name == "score");
  REQUIRE(symbols.lookup(0x0320)->name == "SPEED");
  REQUIRE(symbols.lookup(0x4000)->name == "TABLE");
  REQUIRE(symbols.lookup(0x2000)->category == DebugSymbols::Category::Imported);
  REQUIRE(symbols.resolve("player", 0xFFFF) == 0x0300u);
}

TEST_CASE("Labels and imports survive the settings file", "[debugger][symbols]") {
  DebugSymbols symbols;
  symbols.setLabel(0x0300, "PLAYER");
  symbols.setComment(0x0300, "x then y");
  symbols.importSymbols("TABLE EQU $4000\n");
  std::string out;
  symbols.writeSettings(out);

  DebugSymbols back;
  size_t start = 0;
  while (start < out.size()) {
    const size_t end = out.find('\n', start);
    REQUIRE(back.readSetting(out.substr(start, end - start).c_str()));
    start = end + 1;
  }
  REQUIRE(back.label(0x0300)->name == "PLAYER");
  REQUIRE(back.label(0x0300)->comment == "x then y");
  REQUIRE(back.lookup(0x4000)->name == "TABLE");
}

TEST_CASE("A breakpoint's range is an address, two ends, or names", "[debugger][breakpoints]") {
  DebugSymbols symbols;
  using R = std::pair<uint32_t, uint32_t>;
  REQUIRE(Breakpoints::parseRange("$2000", symbols, 0xFFFF) == R{0x2000, 0x2000});
  REQUIRE(Breakpoints::parseRange("$2000-$20FF", symbols, 0xFFFF) == R{0x2000, 0x20FF});
  REQUIRE(Breakpoints::parseRange("KBD-$C010", symbols, 0xFFFF) == R{0xC000, 0xC010});
  REQUIRE_FALSE(Breakpoints::parseRange("$20FF-$2000", symbols, 0xFFFF)); // backwards
  REQUIRE_FALSE(Breakpoints::parseRange("$2000-", symbols, 0xFFFF));
}

TEST_CASE("A stop is credited to the breakpoint behind it", "[debugger][breakpoints]") {
  Breakpoints list;
  Breakpoint range;
  range.start = 0x2000;
  range.end = 0x20FF;
  list.add(range);
  list.toggleExec(0x2010);
  // The exact address before the range around it.
  REQUIRE(list.execFor(0x2010) == 1);
  REQUIRE(list.execFor(0x2050) == 0);
  REQUIRE(list.execFor(0x3000) == -1);

  Breakpoint write;
  write.kind = Breakpoint::Kind::Write;
  write.start = write.end = 0x0400;
  list.add(write);
  REQUIRE(list.accessFor(0x0400, true) == 2);
  REQUIRE(list.accessFor(0x0400, false) == -1); // a read is not a write

  // A disabled one is credited with nothing.
  list.all()[1].enabled = false;
  REQUIRE(list.execFor(0x2010) == 0);

  // The gutter's toggle takes it away again.
  REQUIRE(list.hasExecAt(0x2010));
  list.toggleExec(0x2010);
  REQUIRE_FALSE(list.hasExecAt(0x2010));
}

TEST_CASE("The core holds exactly the enabled breakpoints", "[debugger][breakpoints]") {
  MachineDebug debug;
  Breakpoints list;
  list.toggleExec(0x2000);
  Breakpoint watch;
  watch.kind = Breakpoint::Kind::Write;
  watch.start = watch.end = 0x0400;
  list.add(watch);
  list.apply(debug, {});
  REQUIRE(debug.hasBreakpoints());
  REQUIRE(debug.hasWatchpoints());

  // The PC reaching it stops the machine...
  REQUIRE(debug.shouldBreakBefore(0x2000, 0xFF));
  debug.clearHits();

  // ...until it is disabled, when the core forgets it.
  list.all()[0].enabled = false;
  list.all()[1].enabled = false;
  list.apply(debug, {});
  REQUIRE_FALSE(debug.hasBreakpoints());
  REQUIRE_FALSE(debug.hasWatchpoints());
  REQUIRE_FALSE(debug.shouldBreakBefore(0x2000, 0xFF));
}

TEST_CASE("Breakpoints survive the settings file", "[debugger][breakpoints]") {
  Breakpoints list;
  Breakpoint b;
  b.kind = Breakpoint::Kind::ReadWrite;
  b.start = 0xC000;
  b.end = 0xC0FF;
  b.condition = "A == $41";
  b.enabled = false;
  list.add(b);
  list.toggleExec(0xE12000);
  std::string out;
  list.writeSettings(out);

  Breakpoints back;
  size_t start = 0;
  while (start < out.size()) {
    const size_t end = out.find('\n', start);
    REQUIRE(back.readSetting(out.substr(start, end - start).c_str()));
    start = end + 1;
  }
  REQUIRE(back.all().size() == 2);
  REQUIRE(back.all()[0].kind == Breakpoint::Kind::ReadWrite);
  REQUIRE(back.all()[0].end == 0xC0FF);
  REQUIRE(back.all()[0].condition == "A == $41");
  REQUIRE_FALSE(back.all()[0].enabled);
  REQUIRE(back.all()[1].start == 0xE12000);
  REQUIRE_FALSE(back.readSetting("Something=else"));
}

TEST_CASE("Switch and beam breakpoints share the list, and the core reports them by id",
          "[debugger][breakpoints]") {
  MachineDebug debug;
  const auto iie = softSwitchCatalog(machineProfile(MachineId::AppleIIe));
  Breakpoints list;
  list.toggleExec(0x2000);
  Breakpoint page2;
  page2.kind = Breakpoint::Kind::Switch;
  page2.key = "page2";
  list.add(page2);
  Breakpoint newVideo; // a IIgs register, which a //e does not have
  newVideo.kind = Breakpoint::Kind::Switch;
  newVideo.key = "newvideo";
  newVideo.equals = true;
  newVideo.value = 0x80;
  newVideo.mask = 0x80;
  list.add(newVideo);
  Breakpoint vbl;
  vbl.kind = Breakpoint::Kind::Beam;
  vbl.beamMode = Breakpoint::BeamVbl;
  vbl.scanline = 192;
  vbl.hPos = 0;
  list.add(vbl);
  REQUIRE_FALSE(list.add(page2)); // the same one twice
  REQUIRE(list.all().size() == 4);

  REQUIRE(list.needsApply());
  list.apply(debug, iie);
  REQUIRE_FALSE(list.needsApply());
  REQUIRE(debug.hasBreakpoints());
  REQUIRE(debug.hasSwitchBreakpoints());
  REQUIRE(debug.hasBeamBreakpoints());
  // Applied, a switch the machine has; kept and not applied, one it has not.
  REQUIRE(list.all()[1].coreId >= 0);
  REQUIRE(list.all()[2].coreId == -1);
  REQUIRE(list.switchFor(list.all()[1].coreId) == 1);
  REQUIRE(list.beamFor(list.all()[3].coreId) == 3);
  REQUIRE(list.switchFor(-1) == -1);

  // PAGE2 changing stops the core, credited to the list's entry.
  uint64_t flags = 0;
  auto read = [&](uint32_t) { return flags; };
  debug.checkSwitches(read, 0x2000);
  flags = 1ULL << 2;
  REQUIRE(debug.checkSwitches(read, 0x2003));
  REQUIRE(list.switchFor(debug.switchBreakpointHitId()) == 1);

  // A condition or a hit count is the host's business and applies nothing;
  // disabling one is the core's.
  list.all()[1].condition = "A == 1";
  list.all()[1].hits = 3;
  REQUIRE_FALSE(list.needsApply());
  list.all()[1].enabled = false;
  REQUIRE(list.needsApply());
  list.apply(debug, iie);
  REQUIRE_FALSE(debug.hasSwitchBreakpoints());

  // A rebuilt machine holds none of it, so the list is applied again.
  list.invalidate();
  REQUIRE(list.needsApply());
  MachineDebug rebuilt;
  list.apply(rebuilt, iie);
  REQUIRE(rebuilt.hasBreakpoints());
  REQUIRE(rebuilt.hasBeamBreakpoints());

  REQUIRE(Breakpoints::describeSwitch(list.all()[1], iie) == "PAGE2 changes");
  REQUIRE(Breakpoints::describeSwitch(list.all()[2], iie) == "NEWVIDEO & $80 = $80");
  const auto iigs = softSwitchCatalog(machineProfile(MachineId::AppleIIgs));
  REQUIRE(Breakpoints::describeSwitch(list.all()[2], iigs) == "NEWVIDEO & $80 = $80");
}

TEST_CASE("Every kind survives the settings file, and the old lines are read",
          "[debugger][breakpoints]") {
  Breakpoints list;
  Breakpoint text;
  text.kind = Breakpoint::Kind::Switch;
  text.key = "text";
  text.equals = true;
  text.value = 1;
  text.condition = "PEEK($24) == 0";
  list.add(text);
  Breakpoint line;
  line.kind = Breakpoint::Kind::Beam;
  line.beamMode = Breakpoint::BeamLineColumn;
  line.scanline = 100;
  line.hPos = 30;
  line.enabled = false;
  list.add(line);
  std::string out;
  list.writeSettings(out);

  Breakpoints back;
  size_t start = 0;
  while (start < out.size()) {
    const size_t end = out.find('\n', start);
    REQUIRE(back.readSetting(out.substr(start, end - start).c_str()));
    start = end + 1;
  }
  REQUIRE(back.all().size() == 2);
  REQUIRE(back.all()[0].kind == Breakpoint::Kind::Switch);
  REQUIRE(back.all()[0].key == "text");
  REQUIRE(back.all()[0].equals);
  REQUIRE(back.all()[0].value == 1);
  REQUIRE(back.all()[0].condition == "PEEK($24) == 0");
  REQUIRE(back.all()[1].kind == Breakpoint::Kind::Beam);
  REQUIRE(back.all()[1].beamMode == Breakpoint::BeamLineColumn);
  REQUIRE(back.all()[1].scanline == 100);
  REQUIRE(back.all()[1].hPos == 30);
  REQUIRE_FALSE(back.all()[1].enabled);

  // What the beam list and the Soft Switches window kept before the list
  // was shared.
  Breakpoints old;
  REQUIRE(old.readSetting("Beam=0\t192\t0\t1"));
  REQUIRE(old.readSetting("SwitchBreakpoint=newvideo\tequals\t128\t128\t1"));
  REQUIRE(old.all().size() == 2);
  REQUIRE(old.all()[0].kind == Breakpoint::Kind::Beam);
  REQUIRE(old.all()[0].scanline == 192);
  REQUIRE(old.all()[1].kind == Breakpoint::Kind::Switch);
  REQUIRE(old.all()[1].mask == 0x80);
  // Read twice (an old line beside its new one), it is still one breakpoint.
  REQUIRE(old.readSetting("SwitchBreakpoint=newvideo\tequals\t128\t128\t1"));
  REQUIRE(old.all().size() == 2);
}

// ---- The rule builder's conditions ----

#include "../src/condition_rules.hpp"
#include "debug/condition_evaluator.hpp"

TEST_CASE("Rules write the expression the browser's builder writes", "[debugger][rules]") {
  ConditionNode root = ConditionNode::group(true);
  ConditionNode a = ConditionNode::rule(ConditionNode::Subject::Register);
  a.value = "$41";
  root.children.push_back(a);
  REQUIRE(toExpression(root) == "A==$41");

  ConditionNode peek = ConditionNode::rule(ConditionNode::Subject::Byte);
  peek.detail = "24";
  peek.op = ">";
  peek.value = "10";
  ConditionNode any = ConditionNode::group(false);
  ConditionNode carry = ConditionNode::rule(ConditionNode::Subject::Flag);
  carry.value = "1";
  ConditionNode i = ConditionNode::rule(ConditionNode::Subject::BasicVar);
  i.detail = "i%";
  i.value = "3";
  any.children = {carry, i};
  root.children.push_back(peek);
  root.children.push_back(any);
  REQUIRE(toExpression(root) == "((A==$41) && (PEEK($0024)>10) && (((C==1) || (BV(201,128)==3))))");

  // Bare hex that is not decimal takes the evaluator's "#$".
  a.value = "ff";
  REQUIRE(toExpression(a) == "A==#$ff");
  // An empty group says nothing, and a group of one is that one.
  REQUIRE(toExpression(ConditionNode::group()).empty());
}

TEST_CASE("A condition reads back into the tree that wrote it", "[debugger][rules]") {
  const std::string written = "((A==$41) && (PEEK($0024)>10) && (((C==1) || (BV(201,128)==3))) && (BA2(65,0,2,3)!=0))";
  const auto tree = fromExpression(written);
  REQUIRE(tree);
  REQUIRE(tree->all);
  REQUIRE(tree->children.size() == 4);
  REQUIRE(tree->children[2].type == ConditionNode::Type::Group);
  REQUIRE_FALSE(tree->children[2].all);
  REQUIRE(tree->children[2].children[1].detail == "I%");
  REQUIRE(tree->children[3].subject == ConditionNode::Subject::BasicArray);
  REQUIRE(tree->children[3].index2 == "3");
  REQUIRE(toExpression(*tree) == written);

  // Typed by hand, in the shape the builder writes.
  const auto typed = fromExpression("a == $41 && x < 5");
  REQUIRE(typed);
  REQUIRE(typed->children.size() == 2);
  REQUIRE(typed->children[1].detail == "X");
  REQUIRE(fromExpression("")->children.empty());

  // Not something the builder could have written.
  REQUIRE_FALSE(fromExpression("A + 1 == 3"));
  REQUIRE_FALSE(fromExpression("A == X"));
  REQUIRE_FALSE(fromExpression("FOO == 1"));
  REQUIRE_FALSE(fromExpression("(A == 1"));
}

TEST_CASE("A condition reads as one line, however many rules it holds", "[debugger][rules]") {
  // The BASIC window labels a rule with the whole tree, as the browser's
  // toDisplayLabel, rather than only when it holds a single rule.
  const auto tree = fromExpression("((BV(201,128)==3) && (((C==1) || (PEEK($0024)>10))))");
  REQUIRE(tree);
  REQUIRE(describe(*tree) == "I% == 3 and (flag C == 1 or PEEK($0024) > 10)");
  REQUIRE(describe(ConditionNode::group()).empty());
}

TEST_CASE("A rule that cannot be evaluated says why", "[debugger][rules]") {
  ConditionNode r = ConditionNode::rule(ConditionNode::Subject::Byte);
  r.value = "1";
  REQUIRE_FALSE(problem(r).empty());
  r.detail = "$C000";
  REQUIRE(problem(r).empty());
  r.value = "lots";
  REQUIRE_FALSE(problem(r).empty());
  ConditionNode v = ConditionNode::rule(ConditionNode::Subject::BasicVar);
  v.detail = "1X";
  v.value = "0";
  REQUIRE_FALSE(problem(v).empty());
  // A name, through the debugger's symbols.
  DebugSymbols symbols;
  ConditionNode ch = ConditionNode::rule(ConditionNode::Subject::Byte);
  ch.detail = "CH";
  ch.value = "0";
  const AddressResolver resolve = [&](const std::string &t) { return symbols.resolve(t, 0xFFFF); };
  REQUIRE(problem(ch, resolve).empty());
  REQUIRE(toExpression(ch, resolve) == "PEEK($0024)==0");
}

TEST_CASE("What the builder writes, the evaluator reads", "[debugger][rules]") {
  uint8_t memory[0x10000] = {};
  memory[0x24] = 12;
  MachineView view;
  view.peek = [&](uint32_t a) { return memory[a & 0xFFFF]; };
  view.a = 0x41;
  view.p = 0x01;
  const auto tree = fromExpression("A == $41 && PEEK($24) > 10 && (C == 0 || X == 0)");
  REQUIRE(tree);
  const std::string expr = toExpression(*tree);
  REQUIRE(ConditionEvaluator::evaluate(expr.c_str(), view));
  view.x = 1;
  REQUIRE_FALSE(ConditionEvaluator::evaluate(expr.c_str(), view));
}

// ---- The console's commands ----

#include "../src/console_command.hpp"

namespace {
using CK = ConsoleCommand::Kind;
ConsoleCommand parse(const char *line) { return parseConsoleCommand(line); }
} // namespace

TEST_CASE("The console reads the Apple II monitor's own syntax", "[debugger][console]") {
  ConsoleCommand c = parse("300");
  REQUIRE(c.kind == CK::Dump);
  REQUIRE(c.from == "$300");
  REQUIRE(c.to.empty());

  c = parse("300.3FF");
  REQUIRE(c.kind == CK::Dump);
  REQUIRE(c.from == "$300");
  REQUIRE(c.to == "$3FF");

  c = parse("300: A9 00 8D");
  REQUIRE(c.kind == CK::Write);
  REQUIRE(c.values == std::vector<std::string>{"$A9", "$00", "$8D"});

  REQUIRE(parse("300L").kind == CK::List);
  c = parse("300L 6");
  REQUIRE(c.kind == CK::List);
  REQUIRE(c.count == 6);
  REQUIRE(parse("300L many").kind == CK::Error);
  REQUIRE(parse("300l").from == "$300");
  c = parse("C600G");
  REQUIRE(c.kind == CK::Go);
  REQUIRE(c.from == "$C600");

  // A IIgs's bank and slash, as its monitor writes it.
  c = parse("E1/2000.20FF");
  REQUIRE(c.kind == CK::Dump);
  REQUIRE(c.from == "E1/2000");
  REQUIRE(c.to == "$20FF");

  // A word that is a command is the command; a dollar makes it memory.
  REQUIRE(parse("be 1").kind == CK::BreakEnable);
  c = parse("$BE");
  REQUIRE(c.kind == CK::Dump);
  REQUIRE(c.from == "$BE");

  REQUIRE(parse("300: ZZ").kind == CK::Error);
  REQUIRE(parse("300.").kind == CK::Error);
  REQUIRE(parse("frobnicate").kind == CK::Error);
  REQUIRE(parse("   ").kind == CK::Empty);
}

TEST_CASE("The console reads its own commands", "[debugger][console]") {
  ConsoleCommand c = parse("m COUT");
  REQUIRE(c.kind == CK::Dump);
  REQUIRE(c.from == "COUT");
  c = parse("m $2000.$20FF");
  REQUIRE(c.from == "$2000");
  REQUIRE(c.to == "$20FF");
  c = parse("m $2000 $20FF");
  REQUIRE(c.to == "$20FF");

  c = parse("w $300 A9 $00 8D");
  REQUIRE(c.kind == CK::Write);
  REQUIRE(c.from == "$300");
  REQUIRE(c.values.size() == 3);

  c = parse("f $2000.$3FFF 00");
  REQUIRE(c.kind == CK::Fill);
  REQUIRE(c.to == "$3FFF");
  REQUIRE(parse("f $2000 00").kind == CK::Error);

  c = parse("l");
  REQUIRE(c.kind == CK::List);
  REQUIRE(c.from.empty());
  c = parse("l HOME 30");
  REQUIRE(c.from == "HOME");
  REQUIRE(c.count == 30);

  REQUIRE(parse("g").kind == CK::Go);
  REQUIRE(parse("c").kind == CK::Go); // continue
  REQUIRE(parse("g $C600").from == "$C600");
  c = parse("s 10");
  REQUIRE(c.kind == CK::Step);
  REQUIRE(c.count == 10);
  REQUIRE(parse("s").count == 0);
  REQUIRE(parse("s many").kind == CK::Error);
  REQUIRE(parse("n").kind == CK::StepOver);
  REQUIRE(parse("finish").kind == CK::StepOut);
  REQUIRE(parse("pause").kind == CK::Pause);
  REQUIRE(parse("until $C600").from == "$C600");

  REQUIRE(parse("r").kind == CK::Registers);
  REQUIRE(parse("r").assignments.empty());
  c = parse("r a=$42 pc = COUT  x= 3");
  REQUIRE(c.kind == CK::Registers);
  using A = std::vector<std::pair<std::string, std::string>>;
  REQUIRE(c.assignments == A{{"a", "$42"}, {"pc", "COUT"}, {"x", "3"}});
  REQUIRE(parse("r q=1").kind == CK::Error);
  REQUIRE(parse("r a").kind == CK::Error);

  c = parse("? PEEK($24) + 1");
  REQUIRE(c.kind == CK::Evaluate);
  REQUIRE(c.text == "PEEK($24) + 1");
  REQUIRE(parse("?").kind == CK::Error);

  REQUIRE(parse("sym COUT").text == "COUT");
  REQUIRE(parse("stack").kind == CK::Stack);
  c = parse("trace 50");
  REQUIRE(c.kind == CK::Trace);
  REQUIRE(c.count == 50);

  c = parse("find A9 ?? 8D");
  REQUIRE(c.kind == CK::Find);
  REQUIRE(c.values == std::vector<std::string>{"$A9", "??", "$8D"});
  c = parse("find \"HELLO WORLD\"");
  REQUIRE(c.text == "HELLO WORLD");
  REQUIRE(parse("find A9 XYZ").kind == CK::Error);

  REQUIRE(parse("reset").kind == CK::Reset);
  REQUIRE(parse("reboot").kind == CK::Reboot);
  REQUIRE(parse("cls").kind == CK::Clear);
  REQUIRE(parse("help bp").text == "bp");
}

TEST_CASE("The console reads every kind of breakpoint", "[debugger][console][breakpoints]") {
  ConsoleCommand c = parse("bp $2000");
  REQUIRE(c.kind == CK::BreakAdd);
  REQUIRE(c.breakpoint.kind == Breakpoint::Kind::Exec);
  REQUIRE(c.from == "$2000");
  REQUIRE(c.to.empty());

  c = parse("bp COUT if A == $C1");
  REQUIRE(c.from == "COUT");
  REQUIRE(c.breakpoint.condition == "A == $C1");

  c = parse("bp w $0400-$07FF");
  REQUIRE(c.breakpoint.kind == Breakpoint::Kind::Write);
  REQUIRE(c.from == "$0400");
  REQUIRE(c.to == "$07FF");
  REQUIRE(parse("bp r KBD").breakpoint.kind == Breakpoint::Kind::Read);
  REQUIRE(parse("bp rw $C000").breakpoint.kind == Breakpoint::Kind::ReadWrite);
  c = parse("bp sp $F0-$FF");
  REQUIRE(c.breakpoint.kind == Breakpoint::Kind::Stack);
  REQUIRE(c.to == "$FF");

  c = parse("bp sw page2");
  REQUIRE(c.breakpoint.kind == Breakpoint::Kind::Switch);
  REQUIRE(c.breakpoint.key == "page2");
  REQUIRE_FALSE(c.breakpoint.equals);
  c = parse("bp sw TEXT off");
  REQUIRE(c.breakpoint.key == "text");
  REQUIRE(c.breakpoint.equals);
  REQUIRE(c.breakpoint.value == 0);
  REQUIRE(parse("bp sw text on").breakpoint.value == 1);
  c = parse("bp sw newvideo&$80=$80");
  REQUIRE(c.breakpoint.key == "newvideo");
  REQUIRE(c.breakpoint.mask == 0x80);
  REQUIRE(c.breakpoint.value == 0x80);
  c = parse("bp sw newvideo & $80 = $C1 if X == 0");
  REQUIRE(c.breakpoint.mask == 0x80);
  REQUIRE(c.breakpoint.value == 0x80); // the value under its mask
  REQUIRE(c.breakpoint.condition == "X == 0");
  c = parse("bp sw border=6");
  REQUIRE(c.breakpoint.mask == 0xFF);
  REQUIRE(c.breakpoint.value == 6);
  REQUIRE(parse("bp sw").kind == CK::Error);
  REQUIRE(parse("bp sw page2 sideways").kind == CK::Error);
  REQUIRE(parse("bp sw newvideo&$00=$00").kind == CK::Error);

  c = parse("bp beam vbl");
  REQUIRE(c.breakpoint.kind == Breakpoint::Kind::Beam);
  REQUIRE(c.breakpoint.beamMode == Breakpoint::BeamVbl);
  c = parse("bp beam line 100");
  REQUIRE(c.breakpoint.beamMode == Breakpoint::BeamLine);
  REQUIRE(c.breakpoint.scanline == 100);
  c = parse("bp beam col 20");
  REQUIRE(c.breakpoint.beamMode == Breakpoint::BeamColumn);
  REQUIRE(c.breakpoint.hPos == 20);
  c = parse("bp beam 100,20");
  REQUIRE(c.breakpoint.beamMode == Breakpoint::BeamLineColumn);
  REQUIRE(c.breakpoint.scanline == 100);
  REQUIRE(c.breakpoint.hPos == 20);
  REQUIRE(parse("bp beam diagonal").kind == CK::Error);

  REQUIRE(parse("bp").kind == CK::Error);
  REQUIRE(parse("bp $2000 $3000").kind == CK::Error);
  REQUIRE(parse("bp $2000 if").kind == CK::Error);

  REQUIRE(parse("bl").kind == CK::BreakList);
  c = parse("bd 2");
  REQUIRE(c.kind == CK::BreakDelete);
  REQUIRE(c.count == 2);
  REQUIRE(parse("bd all").all);
  REQUIRE(parse("bx 1").kind == CK::BreakDisable);
  REQUIRE(parse("bd 0").kind == CK::Error);
  REQUIRE(parse("bd").kind == CK::Error);
}

TEST_CASE("Every command has help, and every alias reaches it", "[debugger][console]") {
  for (const ConsoleCommandHelp &help : consoleCommands()) {
    INFO(help.name);
    REQUIRE(std::strlen(help.usage) > 0);
    REQUIRE(std::strlen(help.summary) > 0);
  }
  REQUIRE(parse("continue").kind == CK::Go);
  REQUIRE(parse("registers").kind == CK::Registers);
  REQUIRE(parse("delete 1").kind == CK::BreakDelete);
  REQUIRE(parse("disable all").kind == CK::BreakDisable);
  REQUIRE(parse("print A").kind == CK::Evaluate);
}
