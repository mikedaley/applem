/*
 * condition_rules.hpp - A breakpoint condition as a tree of rules
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace a2e::native {

// The browser's Rule Builder (rule-builder-window.js) as data: a group of
// rules and further groups, matched ALL or ANY, each rule a comparison of
// something in the machine against a number. It writes the same expression
// the browser writes, which is what ConditionEvaluator reads and what a
// breakpoint stores.
//
// The tree is not stored. A condition is read back into one when the
// builder opens (fromExpression), so the settings keep one string per
// breakpoint, a condition typed by hand opens in the builder when it is the
// shape the builder writes, and the two can never disagree.
struct ConditionNode {
  enum class Type { Group, Rule };
  enum class Subject { Register, Flag, Byte, Word, BasicVar, BasicArray };

  Type type = Type::Group;

  // A group.
  bool all = true; // ALL (&&) or ANY (||)
  std::vector<ConditionNode> children;

  // A rule: subject, detail (a register, a flag, an address or a BASIC
  // variable's name), an array's indices, the comparison and the value.
  Subject subject = Subject::Register;
  std::string detail = "A";
  std::string index1, index2;
  std::string op = "==";
  std::string value;

  static ConditionNode group(bool all = true);
  static ConditionNode rule(Subject subject = Subject::Register);
};

// The comparisons, registers and flags a rule offers, in the order shown.
extern const char *const CONDITION_OPERATORS[6];
extern const char *const CONDITION_REGISTERS[6];
extern const char *const CONDITION_FLAGS[7];

// Turns an address the user typed ("$24", "24", a name) into a number, or
// nothing. The builder hands in the debugger's symbols; without one, only
// hex is read.
using AddressResolver = std::function<std::optional<uint32_t>(const std::string &)>;

// The expression the tree stands for: each rule in parentheses, joined by
// && or || and the group bracketed, as the browser writes it. Empty when
// there is nothing to test. Fields that do not make sense are written as
// best they can be; use problem() first to refuse them.
std::string toExpression(const ConditionNode &node, const AddressResolver &resolve = nullptr, bool wide = false);

// What is wrong with a rule, or empty. Checked over the whole tree: the
// first problem found, so the builder can say what stops it applying.
std::string problem(const ConditionNode &node, const AddressResolver &resolve = nullptr);

// A rule read as people say it: "A == $41", "PEEK($24) > 10", "I% == 3".
std::string describe(const ConditionNode &node);

// The tree behind an expression, always a group at the root (empty for an
// empty expression), or nothing when the expression is not one the builder
// could have written: arithmetic, a register on the right, an unknown name.
std::optional<ConditionNode> fromExpression(const std::string &expression);

// Applesoft's two name bytes for a variable ("I", "SC%", "A$"), and back.
std::pair<uint8_t, uint8_t> encodeBasicName(const std::string &name);
std::string decodeBasicName(uint8_t b1, uint8_t b2);

} // namespace a2e::native
