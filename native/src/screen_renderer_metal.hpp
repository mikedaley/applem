/*
 * screen_renderer_metal.hpp - The CRT chain in Metal
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include "platform.hpp"

#include <string>

namespace a2e::native {

// Built from an id<MTLDevice> and the id<MTLCommandQueue> ImGui's main
// viewport draws with, passed as void* so this header stays C++. Sharing the
// queue orders the CRT pass ahead of the frame that shows it. The shader is
// crt.metal from the app bundle's Resources unless `shaderPath` names one.
std::unique_ptr<ScreenRenderer> makeMetalScreenRenderer(void *device, void *queue,
                                                        const std::string &shaderPath = "");

} // namespace a2e::native
