/*
 * build_stamp.hpp - When this copy of the app was built
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

namespace a2e::native {

// Seconds since 1970, UTC, to the minute: written by the build
// (native/cmake/build_stamp.cmake), so a tester can tell one build from the
// next in the About panel.
extern const long long BUILD_TIME;

} // namespace a2e::native
