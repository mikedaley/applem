// crt.metal - The CRT picture, the phosphor's persistence, and the glass edge
//
// A port of public/shaders/crt.glsl, burnin.glsl and edge.glsl. The browser
// build's comments explain why each effect is the way it is, and the rules
// they state hold here too: the photosensitive-epilepsy limits on anything
// animated (flicker, glowingLine), the mask in physical pixels while the beam
// moves, and an exponential, per-phosphor persistence. Keep the two in step:
// a picture tuned in one build should look the same in the other.
//
// Differences that are only Metal's: texture coordinates start at the top
// left in both passes, so nothing is flipped; fragment positions start at
// the top left too, so the shadow mask turns them back into GL's bottom-up
// rows; and the source sampler is chosen by the host, as the browser chooses
// the texture's filter.
//
// Written by
//  Mike Daley <michael_daley@icloud.com>

#include <metal_stdlib>
using namespace metal;

// Field for field CrtUniforms in crt_params.hpp; every member four bytes.
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
    int maskType;
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
    int monochromeMode;
    float cornerRadius;
    float screenMargin;
    float beamY;
    float beamX;
    float screenInset;
    float edgeHighlight;
    float surround[3];
    float burnInTau;
    float deltaTime;
    float pad[3];
};

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

// A full-screen quad from four vertex ids, uv (0,0) at the top left.
vertex VertexOut crtVertex(uint vid [[vertex_id]]) {
    const float2 corners[4] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1)};
    VertexOut out;
    out.position = float4(corners[vid], 0.0, 1.0);
    out.uv = float2((corners[vid].x + 1.0) * 0.5, (1.0 - corners[vid].y) * 0.5);
    return out;
}

// GLSL's mod, which floors; Metal's fmod truncates.
static float glmod(float x, float y) { return x - y * floor(x / y); }

static float2 res(constant CrtUniforms &u) { return float2(u.resolution[0], u.resolution[1]); }
static float2 texSize(constant CrtUniforms &u) { return float2(u.textureSize[0], u.textureSize[1]); }

// ============================================
// Utility functions
// ============================================

static float hash12(float2 p) {
    float3 p3 = fract(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

// 3D -> 1D hash, the frame counter as the third component so every frame of
// grain is an independent field.
static float hash13(float3 p3) {
    p3 = fract(p3 * 0.1031);
    p3 += dot(p3, p3.zyx + 31.32);
    return fract((p3.x + p3.y) * p3.z);
}

static float rgb2grey(float3 v) { return dot(v, float3(0.21, 0.72, 0.07)); }

// ============================================
// Geometry
// ============================================

static float2 applyOverscan(constant CrtUniforms &u, float2 uv) {
    if (u.overscan < 0.001) return uv;
    // An overscan of 1.0 is a 10% border on each side.
    float borderSize = u.overscan * 0.1;
    float scale = 1.0 - (borderSize * 2.0);
    return (uv - 0.5) / scale + 0.5;
}

static float2 curveUV(constant CrtUniforms &u, float2 uv) {
    if (u.curvature < 0.001) return uv;
    float2 cc = uv - 0.5;
    float dist = dot(cc, cc);
    float distortion = dist * u.curvature * 0.5;
    return uv + cc * distortion;
}

static float2 applyScreenInset(constant CrtUniforms &u, float2 uv) {
    if (u.screenInset < 0.001) return uv;
    return (uv - 0.5) * (1.0 + u.screenInset) + 0.5;
}

static float2 applyScreenMargin(constant CrtUniforms &u, float2 uv) {
    if (u.screenMargin < 0.001) return uv;
    float scale = 1.0 / (1.0 - u.screenMargin * 2.0);
    return (uv - 0.5) * scale + 0.5;
}

// ============================================
// Signal distortions: the beam moves, the mask does not
// ============================================

static float2 applyHorizontalSync(constant CrtUniforms &u, float2 uv, float time) {
    if (u.horizontalSync < 0.001) return uv;
    float randVal = hash12(float2(floor(time * 0.5), 0.0));
    if (randVal > u.horizontalSync) return uv;
    float distortionFreq = mix(4.0, 40.0, hash12(float2(time * 0.1, 1.0)));
    float distortionScale = u.horizontalSync * 0.02 * randVal;
    float wave = sin((uv.y + time * 0.01) * distortionFreq);
    uv.x += wave * distortionScale;
    return uv;
}

static float2 applyJitter(constant CrtUniforms &u, float2 uv, float time) {
    if (u.jitter < 0.001) return uv;
    float2 noiseCoord = uv * 100.0 + float2(time * 10.0, time * 7.0);
    float2 offset = float2(hash12(noiseCoord) - 0.5,
                           hash12(noiseCoord + float2(100.0, 0.0)) - 0.5);
    return uv + offset * u.jitter * 0.005;
}

static float staticNoise(constant CrtUniforms &u, float2 uv, float time) {
    if (u.staticNoise < 0.001) return 0.0;
    float2 grain = floor(uv * texSize(u));
    // A wrapped frame counter keeps the hash inputs small enough that the
    // field does not visibly repeat.
    float frame = glmod(floor(time * 30.0), 1024.0);
    float noise = hash13(float3(grain, frame));
    float2 cc = uv - 0.5;
    float vignette = 1.0 - length(cc) * 0.5;
    return (noise - 0.5) * u.staticNoise * vignette;
}

// Slow, small and continuous, deliberately: the fastest component is 1.25Hz
// and the peak to peak 6%, inside WCAG 2.3.1's three flashes a second and 10%
// luminance change. Do not raise either.
static float flicker(constant CrtUniforms &u, float time) {
    if (u.flicker < 0.001) return 1.0;
    float wobble = sin(time * 4.90) * 0.6 + sin(time * 7.85) * 0.4;
    return 1.0 + wobble * u.flicker * 0.03;
}

static float glowingLine(constant CrtUniforms &u, float2 uv, float time) {
    if (u.glowingLine < 0.001) return 0.0;
    float beamPos = fract(time * 0.05);
    float dist = abs(uv.y - beamPos);
    return smoothstep(0.1, 0.0, dist) * u.glowingLine * 0.3;
}

static float3 applyAmbientLight(constant CrtUniforms &u, float3 color, float2 uv) {
    if (u.ambientLight < 0.001) return color;
    float dist = length(uv - 0.5);
    float ambient = (1.0 - dist) * (1.0 - dist);
    return color + float3(u.ambientLight * ambient * 0.15);
}

// ============================================
// Magnification
// ============================================

// Sharp bilinear: interpolate only across the seam between two source dots
// and keep each dot's interior flat.
static float2 sharpenUV(constant CrtUniforms &u, float2 uv) {
    if (u.sharpness < 0.001) return uv;
    float2 size = texSize(u);
    float2 px = uv * size;
    float2 seam = floor(px + 0.5);
    float2 offset = px - seam;
    float2 span = min(size / max(res(u), float2(1.0)), float2(1.0));
    span = max(span, float2(1e-4));
    float2 hard = clamp(offset / span, -0.5, 0.5);
    return (seam + mix(offset, hard, u.sharpness)) / size;
}

// ============================================
// Scanlines: a Gaussian beam spot that widens with brightness
// ============================================

static float scanlines(constant CrtUniforms &u, float2 uv, float luma) {
    if (u.scanlineIntensity < 0.001) return 1.0;
    // Lines are doubled in the framebuffer, so a scanline is two texel rows.
    float linePos = uv.y * u.textureSize[1] * 0.5;
    float dist = fract(linePos) - 0.5;
    float sigma = mix(0.18, 0.42, u.scanlineWidth);
    sigma *= 1.0 + sqrt(clamp(luma, 0.0, 1.0)) * u.beamBloom * 1.6;
    float x = dist / sigma;
    float profile = exp(-0.5 * x * x);
    return mix(1.0, profile, u.scanlineIntensity);
}

// ============================================
// Shadow mask: a fixed pitch on the glass, in logical pixels
// ============================================

constant float MASK_PITCH = 3.0;

static float3 shadowMask(constant CrtUniforms &u, float2 fragCoord) {
    if (u.shadowMask < 0.001) return float3(1.0);
    // Measured from the bottom, as gl_FragCoord is, so the staggered rows
    // and the gaps between them fall on the same lines as in the browser.
    float2 pos = float2(fragCoord.x, u.resolution[1] - fragCoord.y) / max(u.pixelRatio, 0.001);
    float3 mask;
    if (u.maskType == 1) {
        // Dot triads on a staggered lattice, with a gap between rows.
        float row = floor(pos.y / MASK_PITCH);
        float stagger = glmod(row, 2.0) * 0.5;
        float idx = glmod(floor(pos.x / (MASK_PITCH / 3.0) + stagger * 1.5), 3.0);
        mask = float3(0.7);
        if (idx < 0.5) mask.r = 1.0;
        else if (idx < 1.5) mask.g = 1.0;
        else mask.b = 1.0;
        float withinRow = fract(pos.y / MASK_PITCH);
        float gap = smoothstep(0.0, 0.25, withinRow) * smoothstep(1.0, 0.75, withinRow);
        mask *= mix(0.8, 1.0, gap);
    } else {
        // Aperture grille: continuous vertical stripes.
        float idx = glmod(floor(pos.x / (MASK_PITCH / 3.0)), 3.0);
        mask = float3(0.7);
        if (idx < 0.5) mask.r = 1.0;
        else if (idx < 1.5) mask.g = 1.0;
        else mask.b = 1.0;
    }
    return mix(float3(1.0), mask, u.shadowMask);
}

static float vignette(constant CrtUniforms &u, float2 uv) {
    if (u.vignette < 0.001) return 1.0;
    float dist = length(uv - 0.5);
    return clamp(1.0 - dist * dist * u.vignette * 2.0, 0.0, 1.0);
}

static float3 glow(constant CrtUniforms &u, texture2d<float> tex, sampler s, float2 uv) {
    if (u.glowIntensity < 0.001) return float3(0.0);
    float3 bloom = float3(0.0);
    float spread = u.glowSpread * 0.01;
    for (int x = -1; x <= 1; x++) {
        for (int y = -1; y <= 1; y++) {
            bloom += tex.sample(s, uv + float2(float(x), float(y)) * spread).rgb;
        }
    }
    return bloom / 9.0 * u.glowIntensity;
}

// Misconvergence: clean in the middle, worst in the corners, worse across
// than down, measured from the glass rather than the moving beam.
static float3 rgbShift(constant CrtUniforms &u, texture2d<float> tex, sampler s,
                       float2 uv, float2 screenUV) {
    if (u.rgbOffset < 0.001) return tex.sample(s, uv).rgb;
    float2 dir = screenUV - 0.5;
    float r2 = dot(dir, dir);
    float amount = u.rgbOffset * 0.0045 * r2;
    float2 anisotropy = float2(1.0, 0.75);
    float2 rOffset = dir * amount * anisotropy;
    float2 bOffset = -dir * amount * anisotropy;
    float2 gOffset = dir * amount * anisotropy * -0.15;
    return float3(tex.sample(s, uv + rOffset).r,
                  tex.sample(s, uv + gOffset).g,
                  tex.sample(s, uv + bOffset).b);
}

// Vertical blending between scanlines. The 1-1-2-1-1 kernel cancels the
// common hi-res pattern whose artifact colours alternate every two rows.
static float3 colorBleed(constant CrtUniforms &u, texture2d<float> tex, sampler s,
                         float2 uv, float3 baseColor) {
    if (u.colorBleed < 0.001) return baseColor;
    float texel = 1.0 / u.textureSize[1];
    float3 up2 = tex.sample(s, uv + float2(0.0, -2.0 * texel)).rgb;
    float3 up1 = tex.sample(s, uv + float2(0.0, -1.0 * texel)).rgb;
    float3 dn1 = tex.sample(s, uv + float2(0.0, 1.0 * texel)).rgb;
    float3 dn2 = tex.sample(s, uv + float2(0.0, 2.0 * texel)).rgb;
    float3 blended = (up2 + up1 + baseColor * 2.0 + dn1 + dn2) / 6.0;
    return mix(baseColor, blended, u.colorBleed);
}

static float3 adjustColor(constant CrtUniforms &u, float3 color) {
    color *= u.brightness;
    color = (color - 0.5) * u.contrast + 0.5;
    float gray = rgb2grey(color);
    return mix(float3(gray), color, u.saturation);
}

static float3 applyMonochrome(constant CrtUniforms &u, float3 color) {
    if (u.monochromeMode == 0) return color;
    float gray = rgb2grey(color);
    if (u.monochromeMode == 1) return float3(gray * 0.2, gray * 1.0, gray * 0.2); // P1 green
    if (u.monochromeMode == 2) return float3(gray * 1.0, gray * 0.75, gray * 0.2); // amber
    if (u.monochromeMode == 3) return float3(gray * 1.0, gray * 1.0, gray * 0.9);  // white
    return color;
}

// ============================================
// Edges
// ============================================

static float edgeFade(float2 uv) {
    float2 edge = smoothstep(0.0, 0.005, uv) * smoothstep(0.0, 0.005, 1.0 - uv);
    return mix(0.85, 1.0, edge.x * edge.y);
}

static float smoothEdge(constant CrtUniforms &u, float2 uv) {
    if (u.cornerRadius < 0.001) return 1.0;
    float aspect = u.textureSize[0] / u.textureSize[1];
    float2 centered = uv - 0.5;
    float ry = u.cornerRadius;
    float rx = ry / aspect;
    float2 cornerDist = max(abs(centered) - (0.5 - float2(rx, ry)), 0.0);
    float2 screenDist = cornerDist * float2(aspect, 1.0);
    float corner = length(screenDist) / ry;
    return 1.0 - smoothstep(0.9, 1.0, corner);
}

static float roundedRectAlpha(float2 uv, float radius) {
    if (radius < 0.001) return 1.0;
    float2 cornerDist = abs(uv - 0.5) - (0.5 - radius);
    if (cornerDist.x < 0.0 || cornerDist.y < 0.0) return 1.0;
    float dist = length(cornerDist);
    return 1.0 - smoothstep(radius - 0.005, radius + 0.005, dist);
}

// The bezel: matte plastic, darkened where it meets the glass and in the
// corners, with a fine grain and a highlight on the inner lip. There is
// deliberately no reflection of the screen on it; see crt.glsl.
static float3 bezelShade(constant CrtUniforms &u, float2 uv) {
    float3 bezel = float3(u.surround[0], u.surround[1], u.surround[2]);
    float dist = length(uv - 0.5);
    float2 edgeDist = min(uv, 1.0 - uv);
    float innerShadow = smoothstep(0.0, 0.12, min(edgeDist.x, edgeDist.y));
    bezel *= mix(0.45, 1.0, innerShadow);
    float cornerDark = 1.0 - dist * dist * 0.6;
    bezel *= clamp(cornerDark, 0.5, 1.0);
    float edgeFactor = smoothstep(0.2, 0.7, dist);
    bezel = mix(bezel, bezel * float3(0.92, 0.90, 0.88), edgeFactor * 0.5);
    float2 grainCoord = uv * res(u) * 0.5;
    float grain = hash12(grainCoord + float2(floor(u.time * 0.5))) * 2.0 - 1.0;
    bezel += grain * 0.015;
    float lipDist = min(edgeDist.x, edgeDist.y);
    float lip = smoothstep(0.008, 0.004, lipDist) * smoothstep(0.0, 0.002, lipDist);
    bezel += float3(lip * 0.2);
    return clamp(bezel, 0.0, 1.0);
}

static float3 beamOverlay(constant CrtUniforms &u, float2 uv) {
    if (u.beamY < 0.0 && u.beamX < 0.0) return float3(0.0);
    float lineW = 1.5 / u.textureSize[0];
    float lineH = 1.5 / u.textureSize[1];
    float intensity = 0.0;
    if (u.beamY >= 0.0 && u.beamY <= 1.0) intensity += smoothstep(lineH, 0.0, abs(uv.y - u.beamY)) * 0.6;
    if (u.beamX >= 0.0 && u.beamX <= 1.0) intensity += smoothstep(lineW, 0.0, abs(uv.x - u.beamX)) * 0.6;
    return float3(1.0, 0.0, 0.0) * min(intensity, 1.0);
}

// ============================================
// The CRT pass
// ============================================

fragment float4 crtFragment(VertexOut in [[stage_in]],
                            constant CrtUniforms &u [[buffer(0)]],
                            texture2d<float> source [[texture(0)]],
                            texture2d<float> burnIn [[texture(1)]],
                            sampler sourceSampler [[sampler(0)]],
                            sampler linearSampler [[sampler(1)]]) {
    float2 uv = in.uv;

    // The screen's outline comes from undistorted coordinates: the glass does
    // not wobble, only the beam does.
    float2 stableCurvedUV = applyScreenInset(u, curveUV(u, uv));
    float3 bezel = bezelShade(u, uv);

    float cornerAlpha = roundedRectAlpha(stableCurvedUV, u.cornerRadius);
    if (cornerAlpha < 0.001) return float4(bezel, 1.0);
    float edgeFactor = smoothEdge(u, stableCurvedUV);
    if (edgeFactor < 0.001) return float4(bezel, 1.0);
    if (any(stableCurvedUV < 0.0) || any(stableCurvedUV > 1.0)) return float4(bezel, 1.0);

    float2 distortedUV = applyHorizontalSync(u, uv, u.time);
    distortedUV = applyJitter(u, distortedUV, u.time);
    float2 curvedUV = applyScreenInset(u, curveUV(u, distortedUV));

    float2 contentUV = applyOverscan(u, curvedUV);
    contentUV = applyScreenMargin(u, contentUV);
    contentUV = sharpenUV(u, contentUV);

    bool inMargin = any(contentUV < 0.0) || any(contentUV > 1.0);

    float3 color = float3(0.0);
    if (!inMargin) {
        color = rgbShift(u, source, sourceSampler, contentUV, stableCurvedUV);
        color = colorBleed(u, source, sourceSampler, contentUV, color);
        if (u.burnIn > 0.001) {
            float3 burnInColor = burnIn.sample(linearSampler, contentUV).rgb;
            color = max(color, burnInColor * u.burnIn);
        }
        color += glow(u, source, sourceSampler, contentUV);
    }

    color *= scanlines(u, curvedUV, rgb2grey(color));
    color *= shadowMask(u, in.position.xy);
    color = adjustColor(u, color);
    color = applyMonochrome(u, color);
    color *= vignette(u, curvedUV);
    if (u.curvature > 0.001) color *= edgeFade(stableCurvedUV);
    color *= flicker(u, u.time);
    color += float3(glowingLine(u, curvedUV, u.time));
    color += float3(staticNoise(u, curvedUV, u.time));
    color = applyAmbientLight(u, color, curvedUV);

    {
        float2 beamUV = applyScreenMargin(u, applyOverscan(u, stableCurvedUV));
        float3 beam = beamOverlay(u, beamUV);
        color = mix(color, float3(1.0, 0.0, 0.0), beam.r);
    }

    color = clamp(color, 0.0, 1.0);
    color = mix(bezel, color, cornerAlpha * edgeFactor);
    return float4(color, 1.0);
}

// ============================================
// Phosphor persistence
// ============================================

// Exponential decay against real elapsed time, per phosphor: a colour tube's
// green holds longest and blue fades quickest; a monochrome tube has one
// phosphor and holds longer. Never darker than what is on screen now.
fragment float4 burnInFragment(VertexOut in [[stage_in]],
                               constant CrtUniforms &u [[buffer(0)]],
                               texture2d<float> source [[texture(0)]],
                               texture2d<float> previous [[texture(1)]],
                               sampler sourceSampler [[sampler(0)]],
                               sampler linearSampler [[sampler(1)]]) {
    float3 current = source.sample(sourceSampler, in.uv).rgb;
    float3 before = previous.sample(linearSampler, in.uv).rgb;
    float3 tau = u.monochromeMode == 0 ? u.burnInTau * float3(1.0, 1.6, 0.7)
                                       : float3(u.burnInTau * 1.5);
    float3 decayed = before * exp(-u.deltaTime / max(tau, float3(0.0001)));
    return float4(max(current, decayed), 1.0);
}

// ============================================
// The glass edge, drawn over the picture and untouched by its effects
// ============================================

static float edgeRoundedRectAlpha(constant CrtUniforms &u, float2 uv, float radius) {
    if (radius < 0.001) return 1.0;
    float aspect = u.textureSize[0] / u.textureSize[1];
    float rx = radius / aspect;
    float ry = radius;
    float2 cornerDist = abs(uv - 0.5) - (0.5 - float2(rx, ry));
    if (cornerDist.x < 0.0 || cornerDist.y < 0.0) return 1.0;
    float dist = length(cornerDist * float2(aspect, 1.0));
    return 1.0 - smoothstep(ry - 0.005, ry + 0.005, dist);
}

// A line of constant width in output pixels, straight edge or corner arc.
static float edgeHighlightIntensity(constant CrtUniforms &u, float2 uv, float radius) {
    if (u.edgeHighlight < 0.001) return 0.0;
    float aspect = u.textureSize[0] / u.textureSize[1];
    float rx = radius / aspect;
    float ry = radius;
    float2 centered = abs(uv - 0.5);
    float2 cornerDist = centered - (0.5 - float2(rx, ry));

    float distFromEdge;
    float2 gradDir;
    if (cornerDist.x > 0.0 && cornerDist.y > 0.0) {
        float2 screenDist = cornerDist * float2(aspect, 1.0);
        distFromEdge = radius - length(screenDist);
        gradDir = normalize(screenDist);
    } else if (centered.x - (0.5 - rx) > centered.y - (0.5 - ry)) {
        distFromEdge = 0.5 - centered.x;
        gradDir = float2(1.0, 0.0);
    } else {
        distFromEdge = 0.5 - centered.y;
        gradDir = float2(0.0, 1.0);
    }

    float uvPerPixel = length(gradDir / res(u));
    float lineWidth = 2.5 * uvPerPixel;
    float aa = 0.75 * uvPerPixel;
    float outer = smoothstep(-aa, aa, distFromEdge);
    float inner = smoothstep(lineWidth + aa, lineWidth - aa, distFromEdge);
    return outer * inner * u.edgeHighlight;
}

fragment float4 edgeFragment(VertexOut in [[stage_in]],
                             constant CrtUniforms &u [[buffer(0)]]) {
    float2 curvedUV = applyScreenInset(u, curveUV(u, in.uv));
    if (edgeRoundedRectAlpha(u, curvedUV, u.cornerRadius) < 0.001) return float4(0.0);
    float edgeGlow = edgeHighlightIntensity(u, curvedUV, u.cornerRadius);
    if (edgeGlow < 0.001) return float4(0.0);
    float lineAlpha = edgeGlow / u.edgeHighlight;
    return float4(float3(0.55, 0.55, 0.5) * u.edgeHighlight, lineAlpha);
}
