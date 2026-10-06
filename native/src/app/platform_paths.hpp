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
std::string appSupportDirectory();

// The UI's typeface: SF Mono, from the copy Terminal ships with (static
// cuts, on every Mac), else the system's variable SFNSMono. Empty if neither
// is there, and ImGui keeps its own font. The fonts are read where they are
// installed, never copied into the app.
std::string uiFontPath();

} // namespace a2e::native
