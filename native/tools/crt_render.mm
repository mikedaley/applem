/*
 * crt_render.mm - Render test pictures through the native CRT chain
 *
 * The native shader is a port of the browser's, and the only way to know a
 * port is faithful is to put the same picture through both and compare. This
 * writes, for every monitor preset and one setting with every static effect
 * on, the machine's frame (decoded by that preset's own decoder), the shader
 * parameters as the browser's renderer names them, and what Metal drew.
 * scripts/compare-crt.mjs then draws the same frames with the browser's
 * WebGL renderer and compares the two.
 *
 *   crt_render <crt.metal> <output directory>
 *
 * Only effects that do not move are compared: flicker, jitter, noise, sync
 * and the glowing line are functions of time and would differ by when each
 * side happened to draw.
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "display/display_settings.hpp"
#include "display/no_signal_frame.hpp"
#include "display/screen_renderer_metal.hpp"

#include "../../src/host/machine_host.hpp"
#include "video/video.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace a2e;
using namespace a2e::native;

namespace {

constexpr int OUT_WIDTH = 1232; // about 2.2x, so sampling is not 1:1
constexpr int OUT_HEIGHT = 844;

struct Variant {
  std::string name;
  DisplaySettings settings;
};

std::vector<Variant> variants() {
  std::vector<Variant> list;
  for (const MonitorPreset &preset : monitorPresets()) {
    DisplaySettings settings;
    applyValues(settings, preset.values);
    list.push_back({preset.id, settings});
  }
  // Every static effect on at once, at values no preset uses.
  DisplaySettings all;
  all.curvature = 30;
  all.overscan = 20;
  all.scanlines = 55;
  all.beamBloom = 40;
  all.shadowMask = 50;
  all.maskType = 1;
  all.phosphorGlow = 30;
  all.vignette = 35;
  all.brightness = 90;
  all.contrast = 80;
  all.saturation = 70;
  all.rgbOffset = 60;
  all.ambientLight = 30;
  all.sharpPixels = 0;
  all.sharpness = 60;
  all.colorBleed = 50;
  all.monochromeMode = 0;
  all.colorMode = COLOR_COMPOSITE;
  all.screenInset = 20;
  all.bezelColor = 0x3a4a5c;
  list.push_back({"everything", all});
  DisplaySettings white = all;
  white.monochromeMode = 3;
  white.maskType = 0;
  white.colorMode = COLOR_MONOCHROME;
  list.push_back({"white", white});
  return list;
}

// The //e at its prompt with a picture written straight into its memory.
std::vector<uint8_t> machineFrame(const DisplaySettings &settings, bool hires) {
  host::MachineHost host;
  host.build();
  Emulator &emulator = *host.emulator();
  std::vector<float> audio(800 * 2);
  for (int i = 0; i < 60; i++) host.generateStereoAudioSamples(audio.data(), 800);
  host.warmReset();
  for (int i = 0; i < 30; i++) host.generateStereoAudioSamples(audio.data(), 800);

  Video &video = *host.video();
  if (settings.colorMode != COLOR_MONOCHROME) {
    video.setColorMode(static_cast<VideoColorMode>(settings.colorMode));
  }
  video.setMonochrome(settings.monochromeMode != 0);

  if (hires) {
    for (uint32_t i = 0; i < 0x2000; i++) {
      emulator.writeMemory(static_cast<uint16_t>(0x2000 + i),
                           static_cast<uint8_t>((i * 73) ^ (i >> 7)));
    }
  } else {
    for (uint32_t i = 0; i < 0x400; i++) {
      emulator.writeMemory(static_cast<uint16_t>(0x400 + i), static_cast<uint8_t>(i * 37));
    }
  }
  emulator.writeMemory(0xC050, 0); // graphics
  emulator.writeMemory(0xC052, 0); // full screen
  emulator.writeMemory(0xC054, 0); // page 1
  emulator.writeMemory(hires ? 0xC057 : 0xC056, 0);
  for (int i = 0; i < 4; i++) host.generateStereoAudioSamples(audio.data(), 800);

  const uint8_t *fb = host.framebuffer();
  return std::vector<uint8_t>(fb, fb + host.framebufferSize());
}

bool writeFile(const std::string &path, const std::vector<uint8_t> &bytes) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  return static_cast<bool>(out);
}

// The parameters under the browser renderer's names (webgl-renderer.js).
std::string paramsJson(const CrtParams &p, int cornerRadiusSetting) {
  (void)cornerRadiusSetting;
  std::ostringstream o;
  o << "{\"curvature\":" << p.curvature << ",\"scanlineIntensity\":" << p.scanlineIntensity
    << ",\"scanlineWidth\":" << p.scanlineWidth << ",\"beamBloom\":" << p.beamBloom
    << ",\"sharpness\":" << p.sharpness << ",\"shadowMask\":" << p.shadowMask
    << ",\"maskType\":" << p.maskType << ",\"glowIntensity\":" << p.glowIntensity
    << ",\"glowSpread\":" << p.glowSpread << ",\"brightness\":" << p.brightness
    << ",\"contrast\":" << p.contrast << ",\"saturation\":" << p.saturation
    << ",\"vignette\":" << p.vignette << ",\"rgbOffset\":" << p.rgbOffset
    << ",\"staticNoise\":" << p.staticNoise << ",\"flicker\":" << p.flicker
    << ",\"jitter\":" << p.jitter << ",\"horizontalSync\":" << p.horizontalSync
    << ",\"glowingLine\":" << p.glowingLine << ",\"ambientLight\":" << p.ambientLight
    << ",\"burnIn\":" << p.burnIn << ",\"overscan\":" << p.overscan
    << ",\"colorBleed\":" << p.colorBleed << ",\"monochromeMode\":" << p.monochromeMode
    // The browser decides rounded corners itself, from the curvature and
    // the bezel, so it is given the radius it would have.
    << ",\"cornerRadius\":0.02"
    << ",\"screenMargin\":" << p.screenMargin << ",\"edgeHighlight\":" << p.edgeHighlight
    << ",\"screenInset\":" << p.screenInset << ",\"surroundColor\":[" << p.surround[0] << ","
    << p.surround[1] << "," << p.surround[2] << "]}";
  return o.str();
}

} // namespace

int main(int argc, const char *argv[]) {
  @autoreleasepool {
    if (argc != 3) {
      std::fprintf(stderr, "usage: crt_render <crt.metal> <output directory>\n");
      return 2;
    }
    const std::string shader = argv[1];
    const std::string dir = argv[2];
    [NSFileManager.defaultManager createDirectoryAtPath:[NSString stringWithUTF8String:dir.c_str()]
                            withIntermediateDirectories:YES
                                             attributes:nil
                                                  error:nil];

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLCommandQueue> queue = [device newCommandQueue];

    std::ostringstream manifest;
    manifest << "[";
    bool first = true;
    for (const Variant &variant : variants()) {
      for (const char *pattern : {"lores", "hires", "nosignal"}) {
        std::vector<uint8_t> source =
            std::string(pattern) == "nosignal"
                ? buildNoSignalFrame(560, 384, "IIE ENHANCED")
                : machineFrame(variant.settings, std::string(pattern) == "hires");

        // A fresh renderer for each, so nothing carries over: no
        // persistence, and the time near zero as the browser's is made.
        auto renderer = makeMetalScreenRenderer((__bridge void *)device, (__bridge void *)queue, shader);
        if (!renderer->error().empty()) {
          std::fprintf(stderr, "%s\n", renderer->error().c_str());
          return 1;
        }
        const CrtParams params = crtParamsFor(variant.settings);
        renderer->setParams(params);
        renderer->upload(source.data(), 560, 384);
        renderer->render(OUT_WIDTH, OUT_HEIGHT, 1.0f);
        std::vector<uint8_t> out;
        int width = 0;
        int height = 0;
        if (!renderer->readPixels(out, width, height)) {
          std::fprintf(stderr, "readback failed\n");
          return 1;
        }

        const std::string stem = dir + "/" + pattern + "." + variant.name;
        writeFile(stem + ".src.rgba", source);
        writeFile(stem + ".metal.rgba", out);
        manifest << (first ? "" : ",") << "\n{\"pattern\":\"" << pattern << "\",\"variant\":\""
                 << variant.name << "\",\"srcWidth\":560,\"srcHeight\":384,\"width\":" << width
                 << ",\"height\":" << height << ",\"sharpPixels\":"
                 << (params.sharpPixels ? "true" : "false")
                 << ",\"params\":" << paramsJson(params, 0) << "}";
        first = false;
      }
    }
    manifest << "\n]\n";
    std::ofstream(dir + "/manifest.json") << manifest.str();
    std::printf("wrote %s/manifest.json\n", dir.c_str());
  }
  return 0;
}
