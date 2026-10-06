/*
 * crt_params.hpp - The CRT shader's parameters, as the renderer takes them
 *
 * Written by
 *  Mike Daley <michael_daley@icloud.com>
 */

#pragma once

#include <cstdint>

namespace a2e::native {

// The browser renderer's crtParams (webgl-renderer.js), with its defaults.
// Sliders are 0-1; brightness, contrast and saturation are 1.0 at neutral.
struct CrtParams {
  float curvature = 0.0f;
  float scanlineIntensity = 0.0f;
  float scanlineWidth = 0.25f;
  float beamBloom = 0.6f;
  float sharpness = 0.0f;
  float shadowMask = 0.0f;
  int maskType = 0;
  float glowIntensity = 0.0f;
  float glowSpread = 0.5f;
  float brightness = 1.0f;
  float contrast = 1.0f;
  float saturation = 1.0f;
  float vignette = 0.0f;
  float rgbOffset = 0.0f;
  float staticNoise = 0.0f;
  float flicker = 0.0f;
  float jitter = 0.0f;
  float horizontalSync = 0.0f;
  float glowingLine = 0.0f;
  float ambientLight = 0.0f;
  float burnIn = 0.0f;
  float overscan = 0.0f;
  float colorBleed = 0.0f;
  int monochromeMode = 0;
  float cornerRadius = 0.02f;
  float screenMargin = 0.0f;
  float edgeHighlight = 0.3f;
  float beamY = -1.0f;
  float beamX = -1.0f;
  float screenInset = 0.0f;
  float surround[3] = {0.784f, 0.722f, 0.604f};
  // Sample the source with nearest filtering rather than linear.
  bool sharpPixels = true;

  bool operator==(const CrtParams &) const = default;
};

// The uniform block crt.metal declares as CrtUniforms, field for field.
// Every member is four bytes so the C++ and Metal layouts cannot disagree
// about padding; static_asserted in the renderer.
struct CrtUniforms {
  float resolution[2];
  float textureSize[2];
  float time;
  float curvature;
  float scanlineIntensity;
  float scanlineWidth;
  float beamBloom;
  float sharpness;
  float shadowMask;
  float pixelRatio;
  int32_t maskType;
  float glowIntensity;
  float glowSpread;
  float brightness;
  float contrast;
  float saturation;
  float vignette;
  float flicker;
  float rgbOffset;
  float staticNoise;
  float jitter;
  float horizontalSync;
  float glowingLine;
  float ambientLight;
  float burnIn;
  float overscan;
  float colorBleed;
  int32_t monochromeMode;
  float cornerRadius;
  float screenMargin;
  float beamY;
  float beamX;
  float screenInset;
  float edgeHighlight;
  float surround[3];
  // Burn-in pass only.
  float burnInTau;
  float deltaTime;
  float pad[3];
};

} // namespace a2e::native
