#pragma once

// Triangular-PDF dither, for use wherever we write 8-bit pixels from values we
// computed at higher precision.
//
// Rounding without it is what makes the TV mode background band. A run of
// pixels whose true values are 20.2 through 20.8 all become 20, the next run
// becomes 21, and the eye averages each run and reads the step between them as
// an edge. Noise added after the rounding cannot undo that, because the
// averages are already wrong. Noise added before it makes a growing fraction of
// the pixels round up instead, so the local average follows the true value and
// there is no step left to see.
//
// The noise is triangular rather than uniform because that keeps it independent
// of the value being rounded. Uniform noise leaves a residual that grows and
// shrinks with the fractional part, which is visible as its own faint pattern.
namespace Dither
{
    // Noise for the pixel at (x, y), in [-1, 1]. Deterministic, and tiled, so
    // a whole image costs one table lookup per pixel.
    float noise(int x, int y);

    // Round to 8 bits, turning the rounding error into noise instead of a step.
    // The amplitude is in output levels: 1.0 is the right value when the result
    // is displayed as-is, and callers whose result gets scaled down before it is
    // displayed scale it up to match.
    inline int quantize(float value, int x, int y, float amplitude)
    {
        int result = static_cast<int>(value + noise(x, y) * amplitude + 0.5f);
        return result < 0 ? 0 : (result > 255 ? 255 : result);
    }
}
