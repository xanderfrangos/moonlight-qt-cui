#pragma once

#include "settings/streamingpreferences.h"

#include <cmath>

// Blue noise grain, adapted from Lilium's ReShade blue noise dithering
// (lilium__blue_noise_dithering.fx in EndlesslyFlowering/ReShade_HDR_shaders,
// GPL-3.0). That shader adds several steps of blue noise to an image that is
// already quantized, which hides banding the image already carries. Here the
// same noise is added before the renderer's own dithering quantizer instead of
// replacing it, so the quantizer still reduces the full-precision result to
// the output depth (10-bit SDR to an 8-bit display, for example) exactly as
// it does without grain.
//
// Amplitudes are the peak of the added noise in output steps, on top of the
// half step the quantizer already contributes. The renderers clamp them per
// pixel so the noise never pushes a value outside the output range: a clipped
// excursion would bias the average brightness, so near black and white the
// grain fades out and exact black stays black.
//
// SDR grain is measured in steps of the depth the frame is quantized to. PQ
// grain is sized in 10-bit steps of the display's own gamma 2.2 response,
// scaled to its peak brightness, because those are the steps the panel ends
// up showing. PQ code values are much finer than that in the shadows. The
// noise itself is still added symmetrically in PQ, the domain the quantizer
// reduces, since noise symmetric in gamma would shift the PQ average.
struct DitherGrainPreset
{
    const char* name;
    float sdrHigh;
    float sdrLow;
    // Fraction of the output range below which sdrLow applies
    float sdrLowBelow;
    float hdrHigh;
    float hdrLow;
    float hdrLowBelow;
};

// The display model PQ grain is measured against
static const float k_DitherGrainPanelGamma = 2.2f;
static const float k_DitherGrainPanelLevels = 1023.0f;
// Used when the display does not report its peak brightness
static const float k_DitherGrainDefaultPeakNits = 1000.0f;

inline DitherGrainPreset getDitherGrainPreset(int grainMode)
{
    // The low-strength cutoffs are Lilium's defaults: code value 35 of 255 in
    // SDR, and 25 of 1023 in the display's gamma for HDR.
    DitherGrainPreset preset = { "off", 0.0f, 0.0f, 35.0f / 255.0f,
                                 0.0f, 0.0f, 25.0f / 1023.0f };

    switch (grainMode) {
    default:
    case StreamingPreferences::DG_OFF:
        break;
    case StreamingPreferences::DG_LIGHT:
        preset.name = "light";
        preset.sdrHigh = 0.5f;
        preset.sdrLow = 0.5f;
        preset.hdrHigh = 1.0f;
        preset.hdrLow = 0.5f;
        break;
    case StreamingPreferences::DG_MEDIUM:
        preset.name = "medium";
        preset.sdrHigh = 1.5f;
        preset.sdrLow = 1.0f;
        preset.hdrHigh = 2.5f;
        preset.hdrLow = 1.5f;
        break;
    case StreamingPreferences::DG_STRONG:
        // Lilium's default strengths, less the half step our quantizer adds
        preset.name = "strong";
        preset.sdrHigh = 2.5f;
        preset.sdrLow = 2.0f;
        preset.hdrHigh = 4.5f;
        preset.hdrLow = 2.5f;
        break;
    }

    return preset;
}

// 10000 nits over the display's peak, which maps PQ's normalized linear light
// onto the display's own range. Unreported or implausible peaks fall back to
// a typical HDR display.
inline float getDitherGrainPeakScale(float peakNits)
{
    if (!(peakNits >= 80.0f && peakNits <= 10000.0f)) {
        peakNits = k_DitherGrainDefaultPeakNits;
    }
    return 10000.0f / peakNits;
}

// The display-gamma value of a full 10000 nit PQ signal. Grain is clamped
// below this so it cannot push PQ past its maximum either.
inline float getDitherGrainPanelMax(float peakScale)
{
    return k_DitherGrainPanelLevels * std::pow(peakScale, 1.0f / k_DitherGrainPanelGamma);
}
