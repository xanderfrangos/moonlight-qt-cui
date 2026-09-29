#pragma once

#include "settings/streamingpreferences.h"

// Debanding strength presets, shared by the libplacebo renderer and the D3D11
// port of libplacebo's debanding shader so both look the same. The fields
// mean exactly what they do in libplacebo's pl_deband_params.
struct DebandPreset
{
    const char* name;
    int iterations;
    float threshold;
    float radius;
    float grain;
};

inline DebandPreset getDebandPreset(int debandMode)
{
    // libplacebo's own defaults (PL_DEBAND_DEFAULTS)
    DebandPreset preset = { "medium", 1, 3.0f, 16.0f, 4.0f };

    switch (debandMode) {
    case StreamingPreferences::DB_GRAIN_ONLY:
        // Zero iterations turns this into a pure grain function. It cannot
        // reconstruct a gradient, but the noise still covers contours that
        // half-LSB dithering is too fine to reach.
        preset.name = "grain only";
        preset.iterations = 0;
        break;
    case StreamingPreferences::DB_LIGHT:
        preset.name = "light";
        preset.threshold = 2.0f;
        preset.grain = 2.0f;
        break;
    default:
    case StreamingPreferences::DB_MEDIUM:
        break;
    case StreamingPreferences::DB_STRONG:
        // A second pass widens the radius, which is what finds the broad
        // soft gradients that a single 16px pass walks straight past.
        preset.name = "strong";
        preset.iterations = 2;
        preset.threshold = 4.0f;
        preset.radius = 24.0f;
        preset.grain = 6.0f;
        break;
    }

    return preset;
}
