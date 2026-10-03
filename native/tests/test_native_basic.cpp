/*
 * test_native_basic.cpp - The BASIC editor's language: colours, statements,
 * Format, Renum and completion
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "basic_language.hpp"

#include <string>

using namespace a2e::native::basic;

namespace {

// The kind of the span holding a column.
Kind kindAt(const std::string &line, int column) {
  for (const Span &s : highlight(line)) {
    if (column >= s.start && column < s.start + s.length) return s.kind;
  }
  return Kind::Plain;
}

} // namespace

TEST_CASE("A line is coloured by what each part of it is", "[basic]") {
  const std::string line = "10 FOR I = 1 TO 10: PRINT \"HI\";A$: GOTO 20: REM done";
  REQUIRE(kindAt(line, 0) == Kind::LineNumber);
  REQUIRE(kindAt(line, 3) == Kind::Loop);           // FOR
  REQUIRE(kindAt(line, 7) == Kind::Variable);       // I
  REQUIRE(kindAt(line, 9) == Kind::Operator);       // =
  REQUIRE(kindAt(line, 11) == Kind::Number);        // 1
  REQUIRE(kindAt(line, 18) == Kind::Punctuation);   // :
  REQUIRE(kindAt(line, 20) == Kind::InputOutput);   // PRINT
  REQUIRE(kindAt(line, 27) == Kind::String);        // "HI"
  REQUIRE(kindAt(line, 31) == Kind::Variable);      // A$
  REQUIRE(kindAt(line, 35) == Kind::Flow);          // GOTO
  REQUIRE(kindAt(line, line.size() - 2) == Kind::Comment);

  SECTION("every character is in exactly one span") {
    int covered = 0;
    for (const Span &s : highlight(line)) {
      REQUIRE(s.start == covered);
      covered += s.length;
    }
    REQUIRE(covered == static_cast<int>(line.size()));
  }

  SECTION("a keyword does not run on into a name, but one ending in punctuation needs no gap") {
    REQUIRE(kindAt("10 TOTAL = 1", 3) == Kind::Variable);
    REQUIRE(kindAt("10 HCOLOR=3", 3) == Kind::Graphics);
    REQUIRE(kindAt("10 PR#6", 3) == Kind::Misc);
    REQUIRE(kindAt("10 hgr2", 3) == Kind::Graphics); // HGR2, not HGR then 2
    REQUIRE(highlight("10 hgr2")[2].length == 4); // after the number and its space
  }
}

TEST_CASE("Statements are split where the machine counts them", "[basic]") {
  const std::string line = "20 A=1: PRINT \"A:B\": B=2: REM x:y";
  const auto list = statements(line);
  REQUIRE(list.size() == 4);
  REQUIRE(line.substr(list[0].start, list[0].end - list[0].start) == "A=1");
  REQUIRE(line.substr(list[1].start, list[1].end - list[1].start) == " PRINT \"A:B\"");
  REQUIRE(line.substr(list[3].start, list[3].end - list[3].start) == " REM x:y"); // a colon in a REM is not a split
  REQUIRE(statementAt(line, 0) == -1); // the line number
  REQUIRE(statementAt(line, 3) == 0);
  REQUIRE(statementAt(line, 6) == 0);  // a statement owns its colon
  REQUIRE(statementAt(line, 10) == 1);
  REQUIRE(statements("just text").empty());
}

TEST_CASE("Format orders, aligns, indents and capitalises as the browser does", "[basic]") {
  const std::string source = "100 next i\n\n20 for i = 1 to 3\n30 print \"lower\"\n40 data a,b\n5 rem keep Case";
  REQUIRE(format(source) == "  5 REM keep Case\n"
                            " 20 FOR I = 1 TO 3\n"
                            " 30   PRINT \"lower\"\n"
                            " 40   DATA a,b\n"
                            "100 NEXT I");
  REQUIRE(format(format(source)) == format(source));
}

TEST_CASE("Renum rewrites every target and says where each line went", "[basic]") {
  const std::string source = "5 GOTO 30\n12 ON X GOSUB 30, 5\n30 IF A THEN 12\n31 PRINT \"GOTO 5\": REM GOTO 5";
  const Renumbered r = renumber(source);
  REQUIRE(r.mapping.at(5) == 10);
  REQUIRE(r.mapping.at(12) == 20);
  REQUIRE(r.mapping.at(30) == 30);
  REQUIRE(r.mapping.at(31) == 40);
  REQUIRE(r.source == "10 GOTO 30\n20 ON X GOSUB 30, 10\n30 IF A THEN 20\n40 PRINT \"GOTO 5\": REM GOTO 5");
}

TEST_CASE("A new line takes the next number after the one above it", "[basic]") {
  const std::vector<std::string> lines = {"10 HOME", "  PRINT", "20 END"};
  REQUIRE(nextLineNumber(lines, 0) == 20);
  REQUIRE(nextLineNumber(lines, 1) == 20);
  REQUIRE(nextLineNumber(lines, 2) == 30);
  REQUIRE(nextLineNumber({"no number"}, 0) == 10);
}

TEST_CASE("Completion offers what fits where the cursor is", "[basic]") {
  const std::string program = "10 DIM SC(10)\n20 FOR SCORE = 1 TO 5\n30 INPUT \"NAME\";NA$\n"
                              "40 DEF FN SQ(X) = X*X\n100 GOTO 20\n200 END";
  SECTION("keywords and the program's own names") {
    const auto c = complete(program, "50 PRINT SC");
    REQUIRE(c.size() >= 3);
    REQUIRE(c[0].text == "SC");   // the array, variables first
    REQUIRE(c[1].text == "SCORE");
    bool keyword = false;
    for (const auto &x : c) keyword = keyword || (x.text == "SCALE=" && x.type == Completion::Type::Keyword);
    REQUIRE(keyword);
  }
  SECTION("line numbers after a jump") {
    const auto c = complete(program, "50 GOTO 2");
    REQUIRE(c.empty()); // one character: not yet
    const auto c2 = complete(program, "50 IF X THEN 20");
    REQUIRE(c2.size() == 2);
    REQUIRE(c2[0].text == "20");
    REQUIRE(c2[1].text == "200");
    REQUIRE(c2[0].type == Completion::Type::LineNumber);
  }
  SECTION("functions after FN") {
    const auto c = complete(program, "50 A = FN SQ");
    REQUIRE(c.size() == 1);
    REQUIRE(c[0].syntax == "FN SQ(X)");
  }
  SECTION("nothing inside a string") { REQUIRE(complete(program, "50 PRINT \"SC").empty()); }
  SECTION("the keyword reference") {
    const KeywordInfo *info = keywordInfo("hplot");
    REQUIRE(info);
    REQUIRE(std::string(info->category) == "Hi-Res");
  }
}

TEST_CASE("Errors and reals read as the machine shows them", "[basic]") {
  REQUIRE(errorMessage(0x10) == "SYNTAX ERROR");
  REQUIRE(errorMessage(0x85) == "DIVISION BY ZERO");
  REQUIRE(errorMessage(0x01) == "ERROR 1");
  REQUIRE(formatReal(42) == "42");
  REQUIRE(formatReal(0) == "0");
  REQUIRE(formatReal(3.25) == "3.25");
  REQUIRE(formatReal(1.0 / 3.0) == "0.333333333");
  REQUIRE(formatReal(1.5e-5) == "1.500000e-5");
}
