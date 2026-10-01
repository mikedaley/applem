/*
 * platform_paths.hpp - Where the native app keeps what it remembers
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <string>

namespace a2e::native {

// ~/Library/Application Support/ApplEm Native, created if it is not there.
// Not the Tauri build's folder: the two apps keep different things in
// different shapes, and neither should read the other's.
std::string appSupportDirectory();

} // namespace a2e::native
