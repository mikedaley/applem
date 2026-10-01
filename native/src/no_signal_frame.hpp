/*
 * no_signal_frame.hpp - The picture shown while the machine is switched off
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace a2e::native {

// An ordinary RGBA frame at the machine's size, uploaded as the source
// texture, so it goes through the whole CRT chain as the machine's own video
// does. A port of src/js/display/no-signal-frame.js. It is static on
// purpose: the animated static it replaced in the browser was a full-screen
// flash inside the band that triggers photosensitive epilepsy.
//
// `machine` is the machine as the menu names it, less "Apple " and in capitals
// ("IIE ENHANCED", "IIGS"), since the message names what to switch on.
std::vector<uint8_t> buildNoSignalFrame(int width, int height, const std::string &machine);

// "Apple IIe Enhanced" as the message writes it: "IIE ENHANCED".
std::string noSignalMachineName(const std::string &profileName);

} // namespace a2e::native
