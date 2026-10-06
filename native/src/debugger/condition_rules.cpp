/*
 * condition_rules.cpp - A breakpoint condition as a tree of rules
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "debugger/condition_rules.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace a2e::native {

const char *const CONDITION_OPERATORS[6] = {"==", "!=", "<", ">", "<=", ">="};
const char *const CONDITION_REGISTERS[6] = {"A", "X", "Y", "SP", "PC", "P"};
const char *const CONDITION_FLAGS[7] = {"N", "V", "B", "D", "I", "Z", "C"};

ConditionNode ConditionNode::group(bool all) {
  ConditionNode n;
  n.type = Type::Group;
  n.all = all;
  return n;
}

ConditionNode ConditionNode::rule(Subject subject) {
  ConditionNode n;
  n.type = Type::Rule;
  n.subject = subject;
  n.detail = subject == Subject::Register ? "A" : subject == Subject::Flag ? "C" : "";
  if (subject == Subject::BasicArray) n.index1 = "0";
  return n;
}

namespace {

std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) a++;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) b--;
  return s.substr(a, b - a);
}

std::string upper(std::string s) {
  for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

bool allOf(const std::string &s, int (*test)(int)) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!test(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

bool isHex(const std::string &s) { return allOf(s, std::isxdigit); }
bool isDecimal(const std::string &s) { return allOf(s, std::isdigit); }

// A value as the evaluator reads it: "$41" and "#$41" are hex, bare digits
// are decimal, and bare hex that is not decimal gets its "#$".
std::string normaliseValue(const std::string &typed) {
  const std::string v = trim(typed);
  // A value not yet typed is shown as missing, not as a zero the user never
  // wrote: the builder previews C==? and refuses to apply it (problem()).
  if (v.empty()) return "?";
  if (v.rfind("#$", 0) == 0 || v[0] == '$' || isDecimal(v)) return v;
  if (isHex(v)) return "#$" + v;
  return v;
}

bool valueIsValid(const std::string &typed) {
  const std::string v = trim(typed);
  if (v.rfind("#$", 0) == 0) return isHex(v.substr(2));
  if (!v.empty() && v[0] == '$') return isHex(v.substr(1));
  return isDecimal(v) || isHex(v);
}

std::optional<uint32_t> resolveAddress(const std::string &typed, const AddressResolver &resolve) {
  const std::string t = trim(typed);
  if (t.empty()) return std::nullopt;
  if (resolve) return resolve(t);
  const std::string digits = t[0] == '$' ? t.substr(1) : t;
  if (!isHex(digits) || digits.size() > 6) return std::nullopt;
  return static_cast<uint32_t>(std::strtoul(digits.c_str(), nullptr, 16));
}

std::string formatAddress(uint32_t address, bool wide) {
  char out[16];
  std::snprintf(out, sizeof out, wide && address > 0xFFFF ? "$%06X" : "$%04X", address);
  return out;
}

bool basicNameIsValid(const std::string &typed) {
  std::string name = upper(trim(typed));
  if (!name.empty() && (name.back() == '%' || name.back() == '$')) name.pop_back();
  if (name.empty() || !std::isalpha(static_cast<unsigned char>(name[0]))) return false;
  return allOf(name, std::isalnum);
}

std::string lhsExpression(const ConditionNode &r, const AddressResolver &resolve, bool wide) {
  switch (r.subject) {
  case ConditionNode::Subject::Register:
  case ConditionNode::Subject::Flag:
    return r.detail;
  case ConditionNode::Subject::Byte:
  case ConditionNode::Subject::Word: {
    const auto at = resolveAddress(r.detail, resolve);
    const std::string address = at ? formatAddress(*at, wide) : "$0000";
    return (r.subject == ConditionNode::Subject::Byte ? "PEEK(" : "DEEK(") + address + ")";
  }
  case ConditionNode::Subject::BasicVar: {
    const auto [b1, b2] = encodeBasicName(r.detail.empty() ? "A" : r.detail);
    return "BV(" + std::to_string(b1) + "," + std::to_string(b2) + ")";
  }
  case ConditionNode::Subject::BasicArray: {
    const auto [b1, b2] = encodeBasicName(r.detail.empty() ? "A" : r.detail);
    const std::string i1 = std::to_string(std::atoi(r.index1.c_str()));
    if (!trim(r.index2).empty()) {
      return "BA2(" + std::to_string(b1) + "," + std::to_string(b2) + "," + i1 + "," +
             std::to_string(std::atoi(r.index2.c_str())) + ")";
    }
    return "BA(" + std::to_string(b1) + "," + std::to_string(b2) + "," + i1 + ")";
  }
  }
  return r.detail;
}

// ---- Reading an expression back ----

struct Token {
  enum class Kind { Name, Number, Punct, End } kind;
  std::string text;
};

std::vector<Token> tokenise(const std::string &s, bool &ok) {
  std::vector<Token> out;
  size_t i = 0;
  ok = true;
  while (i < s.size()) {
    const char c = s[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      i++;
    } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
      size_t j = i;
      while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) j++;
      out.push_back({Token::Kind::Name, upper(s.substr(i, j - i))});
      i = j;
    } else if (c == '#' || c == '$' || std::isdigit(static_cast<unsigned char>(c))) {
      size_t j = i;
      if (s[j] == '#') j++;
      const bool hex = j < s.size() && s[j] == '$';
      if (hex) j++;
      const size_t digits = j;
      while (j < s.size() && (hex ? std::isxdigit(static_cast<unsigned char>(s[j]))
                                  : std::isdigit(static_cast<unsigned char>(s[j])))) {
        j++;
      }
      if (j == digits || (c == '#' && !hex)) {
        ok = false;
        return out;
      }
      out.push_back({Token::Kind::Number, upper(s.substr(i, j - i))});
      i = j;
    } else {
      static const char *const PUNCT[] = {"==", "!=", "<=", ">=", "&&", "||", "<", ">", "(", ")", ","};
      bool found = false;
      for (const char *p : PUNCT) {
        const size_t n = std::char_traits<char>::length(p);
        if (s.compare(i, n, p) == 0) {
          out.push_back({Token::Kind::Punct, p});
          i += n;
          found = true;
          break;
        }
      }
      if (!found) {
        ok = false;
        return out;
      }
    }
  }
  out.push_back({Token::Kind::End, ""});
  return out;
}

long numberValue(const std::string &text) {
  if (text.rfind("#$", 0) == 0) return std::strtol(text.c_str() + 2, nullptr, 16);
  if (text[0] == '$') return std::strtol(text.c_str() + 1, nullptr, 16);
  return std::strtol(text.c_str(), nullptr, 10);
}

class Parser {
public:
  explicit Parser(std::vector<Token> tokens) : t_(std::move(tokens)) {}

  std::optional<ConditionNode> parse() {
    auto node = either();
    if (!node || peek().kind != Token::Kind::End) return std::nullopt;
    return node;
  }

private:
  const Token &peek() const { return t_[pos_]; }
  bool accept(const char *punct) {
    if (peek().kind == Token::Kind::Punct && peek().text == punct) {
      pos_++;
      return true;
    }
    return false;
  }

  // A chain of one joiner is one group; a different joiner needs brackets,
  // which is how the builder writes a group inside a group.
  std::optional<ConditionNode> chain(bool all) {
    auto first = all ? primary() : chain(true);
    if (!first) return std::nullopt;
    const char *joiner = all ? "&&" : "||";
    if (!accept(joiner)) return first;
    ConditionNode g = ConditionNode::group(all);
    g.children.push_back(std::move(*first));
    do {
      auto next = all ? primary() : chain(true);
      if (!next) return std::nullopt;
      g.children.push_back(std::move(*next));
    } while (accept(joiner));
    return g;
  }
  std::optional<ConditionNode> either() { return chain(false); }

  std::optional<ConditionNode> primary() {
    if (accept("(")) {
      auto inner = either();
      if (!inner || !accept(")")) return std::nullopt;
      return inner;
    }
    return comparison();
  }

  std::optional<std::vector<long>> arguments(size_t count) {
    std::vector<long> out;
    if (!accept("(")) return std::nullopt;
    for (size_t i = 0; i < count; i++) {
      if (i && !accept(",")) return std::nullopt;
      if (peek().kind != Token::Kind::Number) return std::nullopt;
      out.push_back(numberValue(t_[pos_++].text));
    }
    if (!accept(")")) return std::nullopt;
    return out;
  }

  std::optional<ConditionNode> comparison() {
    if (peek().kind != Token::Kind::Name) return std::nullopt;
    const std::string name = t_[pos_++].text;
    ConditionNode r = ConditionNode::rule();
    auto isOneOf = [&](const char *const *list, size_t n) {
      for (size_t i = 0; i < n; i++) {
        if (name == list[i]) return true;
      }
      return false;
    };
    if (isOneOf(CONDITION_REGISTERS, 6)) {
      r.subject = ConditionNode::Subject::Register;
      r.detail = name;
    } else if (isOneOf(CONDITION_FLAGS, 7)) {
      r.subject = ConditionNode::Subject::Flag;
      r.detail = name;
    } else if (name == "PEEK" || name == "DEEK") {
      if (!accept("(") || peek().kind != Token::Kind::Number) return std::nullopt;
      const long address = numberValue(t_[pos_++].text);
      if (!accept(")")) return std::nullopt;
      r.subject = name == "PEEK" ? ConditionNode::Subject::Byte : ConditionNode::Subject::Word;
      r.detail = formatAddress(static_cast<uint32_t>(address), true);
    } else if (name == "BV" || name == "BA" || name == "BA2") {
      const size_t count = name == "BV" ? 2 : name == "BA" ? 3 : 4;
      auto args = arguments(count);
      if (!args) return std::nullopt;
      r.subject = name == "BV" ? ConditionNode::Subject::BasicVar : ConditionNode::Subject::BasicArray;
      r.detail = decodeBasicName(static_cast<uint8_t>((*args)[0]), static_cast<uint8_t>((*args)[1]));
      r.index1 = count > 2 ? std::to_string((*args)[2]) : "";
      r.index2 = count > 3 ? std::to_string((*args)[3]) : "";
    } else {
      return std::nullopt;
    }
    if (peek().kind != Token::Kind::Punct) return std::nullopt;
    bool found = false;
    for (const char *op : CONDITION_OPERATORS) {
      if (peek().text == op) found = true;
    }
    if (!found) return std::nullopt;
    r.op = t_[pos_++].text;
    if (peek().kind != Token::Kind::Number) return std::nullopt;
    r.value = t_[pos_++].text;
    return r;
  }

  std::vector<Token> t_;
  size_t pos_ = 0;
};

} // namespace

std::pair<uint8_t, uint8_t> encodeBasicName(const std::string &typed) {
  std::string name = upper(trim(typed));
  bool integer = false, string = false;
  if (!name.empty() && name.back() == '%') integer = true, name.pop_back();
  else if (!name.empty() && name.back() == '$') string = true, name.pop_back();
  uint8_t b1 = name.empty() ? 0 : static_cast<uint8_t>(name[0]);
  uint8_t b2 = name.size() > 1 ? static_cast<uint8_t>(name[1]) : 0;
  if (integer) b1 |= 0x80, b2 |= 0x80;
  else if (string) b2 |= 0x80;
  return {b1, b2};
}

std::string decodeBasicName(uint8_t b1, uint8_t b2) {
  std::string name;
  if (b1 & 0x7F) name += static_cast<char>(b1 & 0x7F);
  if (b2 & 0x7F) name += static_cast<char>(b2 & 0x7F);
  if ((b1 & 0x80) && (b2 & 0x80)) name += '%';
  else if (b2 & 0x80) name += '$';
  return name;
}

std::string toExpression(const ConditionNode &node, const AddressResolver &resolve, bool wide) {
  if (node.type == ConditionNode::Type::Rule) {
    return lhsExpression(node, resolve, wide) + node.op + normaliseValue(node.value);
  }
  std::vector<std::string> parts;
  for (const ConditionNode &child : node.children) {
    std::string part = toExpression(child, resolve, wide);
    if (!part.empty()) parts.push_back(std::move(part));
  }
  if (parts.empty()) return "";
  if (parts.size() == 1) return parts[0];
  std::string out = "(";
  for (size_t i = 0; i < parts.size(); i++) {
    if (i) out += node.all ? " && " : " || ";
    out += "(" + parts[i] + ")";
  }
  return out + ")";
}

std::string problem(const ConditionNode &node, const AddressResolver &resolve) {
  if (node.type == ConditionNode::Type::Group) {
    for (const ConditionNode &child : node.children) {
      std::string p = problem(child, resolve);
      if (!p.empty()) return p;
    }
    return "";
  }
  switch (node.subject) {
  case ConditionNode::Subject::Byte:
  case ConditionNode::Subject::Word:
    if (!resolveAddress(node.detail, resolve)) {
      return trim(node.detail).empty() ? "A memory rule needs an address" : "\"" + trim(node.detail) + "\" is not an address";
    }
    break;
  case ConditionNode::Subject::BasicVar:
  case ConditionNode::Subject::BasicArray:
    if (!basicNameIsValid(node.detail)) {
      return trim(node.detail).empty() ? "A BASIC rule needs a variable name"
                                       : "\"" + trim(node.detail) + "\" is not a BASIC variable name";
    }
    if (node.subject == ConditionNode::Subject::BasicArray &&
        (!isDecimal(trim(node.index1)) || (!trim(node.index2).empty() && !isDecimal(trim(node.index2))))) {
      return "An array index is a whole number";
    }
    break;
  default:
    break;
  }
  if (trim(node.value).empty()) return "Every rule needs a value to compare with";
  if (!valueIsValid(node.value)) return "\"" + trim(node.value) + "\" is not a number: use $41, 65 or #$41";
  return "";
}

std::string describe(const ConditionNode &node) {
  if (node.type == ConditionNode::Type::Group) {
    std::vector<std::string> parts;
    for (const ConditionNode &child : node.children) {
      std::string part = describe(child);
      if (part.empty()) continue;
      if (child.type == ConditionNode::Type::Group && child.children.size() > 1) part = "(" + part + ")";
      parts.push_back(std::move(part));
    }
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) out += (i ? (node.all ? " and " : " or ") : "") + parts[i];
    return out;
  }
  std::string lhs;
  switch (node.subject) {
  case ConditionNode::Subject::Register: lhs = node.detail; break;
  case ConditionNode::Subject::Flag: lhs = "flag " + node.detail; break;
  case ConditionNode::Subject::Byte: lhs = "PEEK(" + trim(node.detail) + ")"; break;
  case ConditionNode::Subject::Word: lhs = "DEEK(" + trim(node.detail) + ")"; break;
  case ConditionNode::Subject::BasicVar: lhs = upper(trim(node.detail)); break;
  case ConditionNode::Subject::BasicArray:
    lhs = upper(trim(node.detail)) + "(" + trim(node.index1) +
          (trim(node.index2).empty() ? "" : "," + trim(node.index2)) + ")";
    break;
  }
  return lhs + " " + node.op + " " + (trim(node.value).empty() ? "?" : trim(node.value));
}

std::optional<ConditionNode> fromExpression(const std::string &expression) {
  if (trim(expression).empty()) return ConditionNode::group();
  bool ok = false;
  std::vector<Token> tokens = tokenise(expression, ok);
  if (!ok) return std::nullopt;
  auto node = Parser(std::move(tokens)).parse();
  if (!node) return std::nullopt;
  if (node->type == ConditionNode::Type::Group) return node;
  ConditionNode root = ConditionNode::group();
  root.children.push_back(std::move(*node));
  return root;
}

} // namespace a2e::native
