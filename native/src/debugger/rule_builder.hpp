/*
 * rule_builder.hpp - The Condition Rule Builder, for any breakpoint
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "debugger/condition_rules.hpp"

#include <optional>
#include <string>

namespace a2e::native {

// The browser's Condition Rule Builder (rule-builder-window.js): a
// breakpoint's condition as groups of rules, matched ALL or ANY and nested,
// written into the condition as the expression they stand for. The CPU
// debugger's breakpoints and the BASIC window's both use it.
//
// The tree is read from the condition when the builder opens
// (condition_rules.hpp), so nothing but the condition is kept: one the
// builder cannot read is shown as such, and Apply replaces it.
class RuleBuilder {
public:
  // Open over a condition, with what the condition is for as the title.
  // `basic` is the BASIC window's builder, as the browser's BASIC mode: a
  // rule is about a BASIC variable or array, and new rules start as one.
  // A rule already about something else keeps every subject on offer.
  void open(const std::string &title, const std::string &condition, bool basic = false);
  bool isOpen() const { return open_ || openNext_; }
  // Draw it while it is open, and return the condition once Apply is
  // pressed. Addresses and names are read through `resolve`; `wide` is a
  // IIgs's 24-bit addresses.
  std::optional<std::string> draw(const AddressResolver &resolve, bool wide);

private:
  bool drawGroup(ConditionNode &group, int depth, const AddressResolver &resolve, bool wide);
  bool drawRule(ConditionNode &rule, const AddressResolver &resolve, bool wide);
  // A rule as this builder starts one.
  ConditionNode newRule() const;

  bool openNext_ = false;
  bool open_ = false;
  bool replace_ = false;
  bool basic_ = false;
  std::string title_;
  std::string original_;
  ConditionNode tree_;
};

} // namespace a2e::native
