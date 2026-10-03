/*
 * test_native_debugger.cpp - The debugger's names for addresses and its breakpoint list
 *
 * What the user types into the debugger is read here, so the rules are
 * pinned: "$" always means a number, a name wins over a bare word that
 * happens to be hex, a bank and a slash is a IIgs address, and nothing wider
 * than the machine's bus is accepted. The breakpoint list must hand the core
 * exactly the enabled entries, take back what it handed over, and come back
 * from the settings as it went in.
 */

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "../src/debug_breakpoints.hpp"
#include "../src/debug_symbols.hpp"
#include "debug/machine_debug.hpp"

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
  list.apply(debug);
  REQUIRE(debug.hasBreakpoints());
  REQUIRE(debug.hasWatchpoints());

  // The PC reaching it stops the machine...
  REQUIRE(debug.shouldBreakBefore(0x2000, 0xFF));
  debug.clearHits();

  // ...until it is disabled, when the core forgets it.
  list.all()[0].enabled = false;
  list.all()[1].enabled = false;
  list.apply(debug);
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
