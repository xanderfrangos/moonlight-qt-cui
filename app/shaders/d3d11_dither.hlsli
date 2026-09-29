// Threshold dithering (blue noise or ordered) for the final quantization of
// video to the output depth. Without it, the color conversion's fractional
// result is rounded or truncated somewhere on the way to the panel, which shows
// up as banding in gradients (skies, fog, dark scenes).
//
// Defining DITHER_OUTPUT selects the dithering variant of a video pixel
// shader. The shaders are otherwise identical, so enabling dithering never
// changes the color conversion itself, only the final quantization.
//
// Optional blue noise grain, after Lilium's ReShade blue noise dithering, can
// be added ahead of that quantization. It never replaces it: the quantizer
// below runs the same way whether or not grain is on. See dithergrain.h.

#ifdef DITHER_OUTPUT

#define VIDEO_SHADER_OUTPUT float4

// Per-frame dithering state. It lives in its own constant buffer because the
// phase changes every frame and the grain domain follows the frame's transfer
// function, while CSC_CONST_BUF is only rebuilt when the frame format does.
cbuffer DITHER_FRAME_CONST_BUF : register(b1)
{
    // Temporal dithering phase for this frame, in [0, 1). Zero when temporal
    // dithering is off, which leaves the pattern fixed in screen space.
    float ditherPhase;

    // Grain amplitudes, as the peak of the added noise in steps of the grain
    // domain. grainHigh is zero when grain is off.
    float grainHigh;
    float grainLow;

    // Fraction of the grain domain's range below which grainLow applies
    float grainLowBelow;

    // Nonzero for PQ frames, whose grain is measured in the display's own
    // gamma rather than in PQ code values
    float grainPq;

    // 10000 nits over the display's peak brightness
    float grainPeakScale;

    // The display-gamma value of the PQ maximum, in panel steps
    float grainPanelMax;

    float grainPadding;
};

// Per-pixel thresholds in [0, 1), tiled across the screen. The renderer fills
// this with blue noise or an ordered (Bayer) matrix depending on the selected
// kernel, so one shader serves both. t0-t2 hold the video planes and upscaler
// inputs, so the thresholds live in t3.
Texture2D<float> ditherThresholds : register(t3);

// Blue noise for the grain, in [0, 1). The same texture as t3 when the kernel
// is blue noise; grain always uses blue noise, even with the ordered kernel.
Texture2D<float> ditherGrainNoise : register(t4);

// Must match k_DitherGrainPanelGamma and k_DitherGrainPanelLevels
static const float k_GrainPanelGamma = 2.2f;
static const float k_GrainPanelLevels = 1023.0f;

// SMPTE ST 2084, normalized so 1.0 is 10000 nits
static const float k_PqM1 = 0.1593017578125f;
static const float k_PqM2 = 78.84375f;
static const float k_PqC1 = 0.8359375f;
static const float k_PqC2 = 18.8515625f;
static const float k_PqC3 = 18.6875f;

float3 pqToLinear(float3 pq)
{
    float3 p = pow(pq, 1.0f / k_PqM2);
    return pow(max(p - k_PqC1, 0.0f) / (k_PqC2 - k_PqC3 * p), 1.0f / k_PqM1);
}

float3 linearToPq(float3 lin)
{
    float3 p = pow(max(lin, 0.0f), k_PqM1);
    return pow((k_PqC1 + k_PqC2 * p) / (1.0f + k_PqC3 * p), k_PqM2);
}

// PQ value of a display-gamma value in panel steps
float3 panelToPq(float3 panel)
{
    return linearToPq(pow(max(panel / k_GrainPanelLevels, 0.0f), k_GrainPanelGamma) / grainPeakScale);
}

// Adds grain to an unquantized color in [0, 1]. levels is the quantizer's, so
// SDR grain is measured in the steps the frame is actually reduced to.
float3 applyDitherGrain(float3 color, uint2 pixel, float levels)
{
    // Read the noise half a tile away from the quantizer's threshold so the
    // two don't line up. Blue noise has almost no correlation at that
    // distance. The phase rotates it per frame like the thresholds.
    uint width, height;
    ditherGrainNoise.GetDimensions(width, height);
    uint2 texel = (pixel + uint2(width, height) / 2) % uint2(width, height);
    float noise = frac(ditherGrainNoise.Load(int3(texel, 0)) + ditherPhase) * 2.0f - 1.0f;

    // Weaker grain in the shadows, as a peak amplitude in [0, 1] units
    float3 amplitude;
    if (grainPq != 0.0f) {
        // Size the grain in the display's own steps, but add it symmetrically
        // in PQ, which is what the quantizer reduces. Noise that's symmetric
        // in the display's gamma isn't in PQ, and would shift the average.
        float3 panel = pow(max(pqToLinear(color) * grainPeakScale, 0.0f), 1.0f / k_GrainPanelGamma) * k_GrainPanelLevels;
        float3 steps = panel <= grainLowBelow * k_GrainPanelLevels ? grainLow : grainHigh;
        steps = max(min(steps, min(panel, grainPanelMax - panel)), 0.0f);
        amplitude = (panelToPq(panel + steps) - panelToPq(panel - steps)) * 0.5f;
    }
    else {
        float3 steps = color <= grainLowBelow ? grainLow : grainHigh;
        amplitude = steps / levels;
    }

    // Never past either end of the range. A clipped excursion would shift the
    // average, so the grain fades out toward black and white instead, and
    // black stays black. Channels with no amplitude come back exactly.
    amplitude = max(min(amplitude, min(color, 1.0f - color)), 0.0f);

    // One noise value for all three channels, as Lilium does, so the grain
    // is luminance noise rather than colored speckle
    return color + noise * amplitude;
}

float3 addDitherGrain(float3 color, uint2 pixel, float levels)
{
    float3 result = color;

    // Uniform across the draw, so grain off costs one untaken branch
    [branch]
    if (grainHigh > 0.0f) {
        result = applyDitherGrain(color, pixel, levels);
    }

    return result;
}

// levels is (2^bitsPerComponent - 1) for the depth this frame is quantized to.
// For the video shaders it arrives in the tail of CSC_CONST_BUF.
float3 orderedDither(min16float3 color, float2 screenPos, float levels)
{
    uint width, height;
    ditherThresholds.GetDimensions(width, height);
    uint2 pixel = uint2(screenPos);
    uint2 texel = pixel % uint2(width, height);

    // The thresholds already carry half a step of offset, which keeps the
    // pattern centered on the rounded value. Advancing the whole set by
    // ditherPhase and wrapping rotates it without disturbing its distribution,
    // so each frame stays a valid dither while the per-pixel threshold
    // decorrelates over time.
    float threshold = frac(ditherThresholds.Load(int3(texel, 0)) + ditherPhase);

    // Grain stays inside [0, 1], so it never changes the quantization below
    float3 grained = addDitherGrain(saturate(float3(color)), pixel, levels);

    // Quantize at full precision. The result is already an exact output-depth
    // value, so the truncation further down the display pipeline is a no-op.
    return floor(grained * levels + threshold) / levels;
}

#define FINISH_VIDEO_SHADER(rgb, screenPos) \
    float4(orderedDither(rgb, screenPos, ditherLevels), 1.0f)

#else

#define VIDEO_SHADER_OUTPUT min16float4
#define FINISH_VIDEO_SHADER(rgb, screenPos) min16float4(rgb, 1.0)

#endif
