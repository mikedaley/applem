/*
 * screen_surface_metal.mm - The screen as a Metal texture
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "screen_surface_metal.hpp"

#import <Metal/Metal.h>

namespace a2e::native {

namespace {

// Three textures, used in turn, one for each frame Metal may have in
// flight. A frame is copied in by the CPU while the GPU may still be drawing
// earlier ones from the others, so the upload never lands in a texture that
// is in use.
class MetalScreenSurface final : public ScreenSurface {
public:
  explicit MetalScreenSurface(id<MTLDevice> device) : device_(device) {}

  void upload(const uint8_t *rgba, int width, int height) override {
    if (width != width_ || height != height_) {
      MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
          texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                       width:width
                                      height:height
                                   mipmapped:NO];
      descriptor.usage = MTLTextureUsageShaderRead;
      for (auto &texture : textures_) texture = [device_ newTextureWithDescriptor:descriptor];
      width_ = width;
      height_ = height;
    }
    current_ = (current_ + 1) % 3;
    [textures_[current_] replaceRegion:MTLRegionMake2D(0, 0, width, height)
                           mipmapLevel:0
                             withBytes:rgba
                           bytesPerRow:width * 4];
  }

  ImTextureID texture() const override {
    if (!textures_[current_]) return ImTextureID_Invalid;
    return (ImTextureID)(intptr_t)(__bridge void *)textures_[current_];
  }

  int width() const override { return width_; }
  int height() const override { return height_; }

private:
  id<MTLDevice> device_;
  id<MTLTexture> textures_[3] = {nil, nil, nil};
  int current_ = 0;
  int width_ = 0;
  int height_ = 0;
};

} // namespace

std::unique_ptr<ScreenSurface> makeMetalScreenSurface(void *device) {
  return std::make_unique<MetalScreenSurface>((__bridge id<MTLDevice>)device);
}

} // namespace a2e::native
