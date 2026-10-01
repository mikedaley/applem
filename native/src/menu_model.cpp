/*
 * menu_model.cpp - The menu bar, described in C++ and built by Cocoa
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "menu_model.hpp"

namespace a2e::native {

namespace {

void sign(const MenuItem &item, std::string &out) {
  out += item.separator ? "-" : item.title;
  out += '\x1f';
  out += item.action;
  out += '\x1f';
  out += item.key;
  out += static_cast<char>('0' + item.modifiers);
  out += item.checked ? 'c' : '_';
  out += item.enabled ? 'e' : '_';
  out += '{';
  for (const MenuItem &child : item.children) sign(child, out);
  out += '}';
}

} // namespace

std::string menuSignature(const MenuBar &bar) {
  std::string out;
  for (const MenuItem &menu : bar) sign(menu, out);
  return out;
}

} // namespace a2e::native
