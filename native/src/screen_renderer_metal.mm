/*
 * screen_renderer_metal.mm - The CRT chain in Metal
 *
 * The browser's WebGLRenderer, pass for pass: every fourth frame the phosphor
 * persistence is accumulated into one of a ping-pong pair at the machine's
 * resolution, then the CRT pass draws the picture at the size it will be
 * shown, and the glass edge is blended over it. The result goes to an
 * offscreen texture that ImGui draws one to one, so a Screen window dragged
 * onto another display is rendered at that display's density.
 *
 * The shader is crt.metal in the bundle's Resources, compiled when the app
 * starts, so it needs no Metal toolchain to build.
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#include "screen_renderer_metal.hpp"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <algorithm>
#include <cstring>

namespace a2e::native {

static_assert(sizeof(CrtUniforms) == 44 * 4, "CrtUniforms must match crt.metal");

namespace {

// Frames Metal may have in flight. The output texture rotates through this
// many, because the secondary viewports draw from their own command queues
// and so are not ordered against the next CRT pass.
constexpr int IN_FLIGHT = 3;

class MetalScreenRenderer final : public ScreenRenderer {
public:
  MetalScreenRenderer(id<MTLDevice> device, id<MTLCommandQueue> queue, const std::string &shaderPath)
      : device_(device), queue_(queue), start_(CACurrentMediaTime()) {
    buildPipelines(shaderPath);
    MTLSamplerDescriptor *descriptor = [MTLSamplerDescriptor new];
    descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
    descriptor.minFilter = MTLSamplerMinMagFilterNearest;
    descriptor.magFilter = MTLSamplerMinMagFilterNearest;
    nearest_ = [device_ newSamplerStateWithDescriptor:descriptor];
    descriptor.minFilter = MTLSamplerMinMagFilterLinear;
    descriptor.magFilter = MTLSamplerMinMagFilterLinear;
    linear_ = [device_ newSamplerStateWithDescriptor:descriptor];
  }

  void upload(const uint8_t *rgba, int width, int height) override {
    const size_t bytes = static_cast<size_t>(width) * height * 4;
    staging_ = (staging_ + 1) % IN_FLIGHT;
    if (!stagingBuffers_[staging_] || stagingBuffers_[staging_].length < bytes) {
      stagingBuffers_[staging_] = [device_ newBufferWithLength:bytes
                                                       options:MTLResourceStorageModeShared];
    }
    std::memcpy(stagingBuffers_[staging_].contents, rgba, bytes);
    pendingWidth_ = width;
    pendingHeight_ = height;
    pending_ = true;
  }

  void setParams(const CrtParams &params) override { params_ = params; }

  void clearPersistence() override { clearBurnIn_ = true; }

  std::string error() const override { return error_; }

  bool readPixels(std::vector<uint8_t> &rgba, int &width, int &height) override {
    id<MTLTexture> texture = outputs_[output_];
    if (!texture) return false;
    width = static_cast<int>(texture.width);
    height = static_cast<int>(texture.height);
    const NSUInteger rowBytes = static_cast<NSUInteger>(width) * 4;
    id<MTLBuffer> buffer = [device_ newBufferWithLength:rowBytes * height
                                                options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> commands = [queue_ commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromTexture:texture
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(width, height, 1)
                 toBuffer:buffer
        destinationOffset:0
   destinationBytesPerRow:rowBytes
 destinationBytesPerImage:rowBytes * height];
    [blit endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    // The output is BGRA.
    const uint8_t *bgra = static_cast<const uint8_t *>(buffer.contents);
    rgba.resize(rowBytes * height);
    for (size_t i = 0; i < rgba.size(); i += 4) {
      rgba[i] = bgra[i + 2];
      rgba[i + 1] = bgra[i + 1];
      rgba[i + 2] = bgra[i];
      rgba[i + 3] = bgra[i + 3];
    }
    return true;
  }

  ImTextureID render(int pixelWidth, int pixelHeight, float pixelRatio) override {
    if (!crtPipeline_ || pixelWidth <= 0 || pixelHeight <= 0) return ImTextureID_Invalid;
    if (!pending_ && !source_) return ImTextureID_Invalid;

    id<MTLCommandBuffer> commands = [queue_ commandBuffer];
    commands.label = @"CRT";

    if (pending_) encodeUpload(commands);
    if (clearBurnIn_) encodeClearBurnIn(commands);

    const double now = CACurrentMediaTime();
    CrtUniforms uniforms = uniformsFor(pixelWidth, pixelHeight, pixelRatio, now - start_);

    // Persistence, throttled to every fourth frame as in the browser, and
    // decayed against the real time since the last pass rather than by a
    // fixed amount, so it does not depend on the frame rate.
    if (++frameCount_ % 4 == 0 && params_.burnIn >= 0.001f) {
      const double dt = lastBurnIn_ < 0 ? 1.0 / 15.0 : std::min(now - lastBurnIn_, 1.0);
      lastBurnIn_ = now;
      uniforms.burnInTau = 0.06f + params_.burnIn * 0.9f;
      uniforms.deltaTime = static_cast<float>(dt);
      encodeBurnIn(commands, uniforms);
    }

    output_ = (output_ + 1) % IN_FLIGHT;
    ensureOutput(output_, pixelWidth, pixelHeight);

    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = outputs_[output_];
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:crtPipeline_];
    [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    [encoder setFragmentTexture:source_ atIndex:0];
    [encoder setFragmentTexture:burnIn_[burnInIndex_] atIndex:1];
    [encoder setFragmentSamplerState:(params_.sharpPixels ? nearest_ : linear_) atIndex:0];
    [encoder setFragmentSamplerState:linear_ atIndex:1];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];

    // The glass edge, unaffected by anything the picture went through.
    if (params_.edgeHighlight > 0.001f) {
      [encoder setRenderPipelineState:edgePipeline_];
      [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    }
    [encoder endEncoding];
    [commands commit];

    return (ImTextureID)(intptr_t)(__bridge void *)outputs_[output_];
  }

private:
  void buildPipelines(const std::string &shaderPath) {
    NSString *path = shaderPath.empty()
                         ? [NSBundle.mainBundle pathForResource:@"crt" ofType:@"metal"]
                         : [NSString stringWithUTF8String:shaderPath.c_str()];
    NSError *failure = nil;
    NSString *source = path ? [NSString stringWithContentsOfFile:path
                                                       encoding:NSUTF8StringEncoding
                                                          error:&failure]
                            : nil;
    if (!source) {
      error_ = "crt.metal is missing from the app bundle";
      return;
    }
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.mathMode = MTLMathModeSafe;
    id<MTLLibrary> library = [device_ newLibraryWithSource:source options:options error:&failure];
    if (!library) {
      error_ = std::string("crt.metal: ") + failure.localizedDescription.UTF8String;
      NSLog(@"%s", error_.c_str());
      return;
    }
    id<MTLFunction> vertex = [library newFunctionWithName:@"crtVertex"];

    auto pipeline = [&](NSString *fragment, MTLPixelFormat format, bool blend) {
      MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
      descriptor.vertexFunction = vertex;
      descriptor.fragmentFunction = [library newFunctionWithName:fragment];
      descriptor.colorAttachments[0].pixelFormat = format;
      if (blend) {
        descriptor.colorAttachments[0].blendingEnabled = YES;
        descriptor.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        descriptor.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        descriptor.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
        descriptor.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
      }
      NSError *pipelineFailure = nil;
      id<MTLRenderPipelineState> state =
          [device_ newRenderPipelineStateWithDescriptor:descriptor error:&pipelineFailure];
      if (!state) {
        error_ = std::string("crt.metal: ") + pipelineFailure.localizedDescription.UTF8String;
        NSLog(@"%s", error_.c_str());
      }
      return state;
    };
    crtPipeline_ = pipeline(@"crtFragment", MTLPixelFormatBGRA8Unorm, false);
    edgePipeline_ = pipeline(@"edgeFragment", MTLPixelFormatBGRA8Unorm, true);
    burnInPipeline_ = pipeline(@"burnInFragment", MTLPixelFormatRGBA8Unorm, false);
    if (!crtPipeline_ || !edgePipeline_ || !burnInPipeline_) crtPipeline_ = nil;
  }

  id<MTLTexture> makeTexture(int width, int height, MTLPixelFormat format, MTLTextureUsage usage) {
    MTLTextureDescriptor *descriptor =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                           width:width
                                                          height:height
                                                       mipmapped:NO];
    descriptor.usage = usage;
    descriptor.storageMode = MTLStorageModePrivate;
    return [device_ newTextureWithDescriptor:descriptor];
  }

  // The machine's picture into the source texture. A different size, which
  // is a different machine, rebuilds the source and the persistence pair.
  void encodeUpload(id<MTLCommandBuffer> commands) {
    pending_ = false;
    if (pendingWidth_ != sourceWidth_ || pendingHeight_ != sourceHeight_) {
      sourceWidth_ = pendingWidth_;
      sourceHeight_ = pendingHeight_;
      source_ = makeTexture(sourceWidth_, sourceHeight_, MTLPixelFormatRGBA8Unorm,
                            MTLTextureUsageShaderRead);
      for (auto &texture : burnIn_) {
        texture = makeTexture(sourceWidth_, sourceHeight_, MTLPixelFormatRGBA8Unorm,
                              MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
      }
      clearBurnIn_ = true;
    }
    id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
    [blit copyFromBuffer:stagingBuffers_[staging_]
             sourceOffset:0
        sourceBytesPerRow:sourceWidth_ * 4
      sourceBytesPerImage:static_cast<NSUInteger>(sourceWidth_) * sourceHeight_ * 4
               sourceSize:MTLSizeMake(sourceWidth_, sourceHeight_, 1)
                toTexture:source_
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    [blit endEncoding];
  }

  void encodeClearBurnIn(id<MTLCommandBuffer> commands) {
    clearBurnIn_ = false;
    lastBurnIn_ = -1;
    for (auto &texture : burnIn_) {
      if (!texture) continue;
      MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = texture;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      [[commands renderCommandEncoderWithDescriptor:pass] endEncoding];
    }
  }

  void encodeBurnIn(id<MTLCommandBuffer> commands, const CrtUniforms &uniforms) {
    const int previous = burnInIndex_;
    burnInIndex_ = 1 - burnInIndex_;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = burnIn_[burnInIndex_];
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:burnInPipeline_];
    [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    [encoder setFragmentTexture:source_ atIndex:0];
    [encoder setFragmentTexture:burnIn_[previous] atIndex:1];
    [encoder setFragmentSamplerState:(params_.sharpPixels ? nearest_ : linear_) atIndex:0];
    [encoder setFragmentSamplerState:linear_ atIndex:1];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [encoder endEncoding];
  }

  void ensureOutput(int index, int width, int height) {
    id<MTLTexture> texture = outputs_[index];
    if (texture && static_cast<int>(texture.width) == width &&
        static_cast<int>(texture.height) == height) {
      return;
    }
    outputs_[index] = makeTexture(width, height, MTLPixelFormatBGRA8Unorm,
                                  MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
  }

  CrtUniforms uniformsFor(int width, int height, float pixelRatio, double time) const {
    const CrtParams &p = params_;
    CrtUniforms u = {};
    u.resolution[0] = static_cast<float>(width);
    u.resolution[1] = static_cast<float>(height);
    u.textureSize[0] = static_cast<float>(sourceWidth_);
    u.textureSize[1] = static_cast<float>(sourceHeight_);
    u.time = static_cast<float>(time);
    u.curvature = p.curvature;
    u.scanlineIntensity = p.scanlineIntensity;
    u.scanlineWidth = p.scanlineWidth;
    u.beamBloom = p.beamBloom;
    u.sharpness = p.sharpness;
    u.shadowMask = p.shadowMask;
    u.pixelRatio = pixelRatio;
    u.maskType = p.maskType;
    u.glowIntensity = p.glowIntensity;
    u.glowSpread = p.glowSpread;
    u.brightness = p.brightness;
    u.contrast = p.contrast;
    u.saturation = p.saturation;
    u.vignette = p.vignette;
    u.flicker = p.flicker;
    u.rgbOffset = p.rgbOffset;
    u.staticNoise = p.staticNoise;
    u.jitter = p.jitter;
    u.horizontalSync = p.horizontalSync;
    u.glowingLine = p.glowingLine;
    u.ambientLight = p.ambientLight;
    u.burnIn = p.burnIn;
    u.overscan = p.overscan;
    u.colorBleed = p.colorBleed;
    u.monochromeMode = p.monochromeMode;
    u.cornerRadius = p.cornerRadius;
    u.screenMargin = p.screenMargin;
    u.beamY = p.beamY;
    u.beamX = p.beamX;
    u.screenInset = p.screenInset;
    u.edgeHighlight = p.edgeHighlight;
    std::copy(std::begin(p.surround), std::end(p.surround), u.surround);
    return u;
  }

  id<MTLDevice> device_;
  id<MTLCommandQueue> queue_;
  id<MTLRenderPipelineState> crtPipeline_ = nil;
  id<MTLRenderPipelineState> edgePipeline_ = nil;
  id<MTLRenderPipelineState> burnInPipeline_ = nil;
  id<MTLSamplerState> nearest_ = nil;
  id<MTLSamplerState> linear_ = nil;
  std::string error_;

  id<MTLBuffer> stagingBuffers_[IN_FLIGHT] = {nil, nil, nil};
  int staging_ = 0;
  bool pending_ = false;
  int pendingWidth_ = 0;
  int pendingHeight_ = 0;

  id<MTLTexture> source_ = nil;
  int sourceWidth_ = 0;
  int sourceHeight_ = 0;
  id<MTLTexture> burnIn_[2] = {nil, nil};
  int burnInIndex_ = 0;
  bool clearBurnIn_ = false;
  double lastBurnIn_ = -1;

  id<MTLTexture> outputs_[IN_FLIGHT] = {nil, nil, nil};
  int output_ = 0;

  CrtParams params_;
  uint64_t frameCount_ = 0;
  double start_;
};

} // namespace

std::unique_ptr<ScreenRenderer> makeMetalScreenRenderer(void *device, void *queue,
                                                        const std::string &shaderPath) {
  return std::make_unique<MetalScreenRenderer>((__bridge id<MTLDevice>)device,
                                               (__bridge id<MTLCommandQueue>)queue, shaderPath);
}

} // namespace a2e::native
