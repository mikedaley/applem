/*
 * screen_surface_metal.hpp - The screen as a Metal texture
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "platform.hpp"

namespace a2e::native {

// Built from an id<MTLDevice> passed as void* so this header stays C++.
std::unique_ptr<ScreenSurface> makeMetalScreenSurface(void *device);

} // namespace a2e::native
