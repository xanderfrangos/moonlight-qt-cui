// Threshold dithering (blue noise or ordered) for the final quantization of
// video to the output depth. Without it, the color conversion's fractional
// result is rounded or truncated somewhere on the way to the panel, which shows
// up as banding in gradients (skies, fog, dark scenes).
//
// Defining DITHER_OUTPUT selects the dithering variant of a video pixel
// shader. The shaders are otherwise identical, so enabling dithering never
// changes the color conversion itself, only the final quantization.

#ifdef DITHER_OUTPUT

#define VIDEO_SHADER_OUTPUT float4

// Temporal dithering phase for this frame, in [0, 1). Zero when temporal
// dithering is off, which leaves the pattern fixed in screen space. It lives in
// its own constant buffer because it changes every frame, while CSC_CONST_BUF
// is only rebuilt when the frame format does.
cbuffer DITHER_FRAME_CONST_BUF : register(b1)
{
    float ditherPhase;
};

// Per-pixel thresholds in [0, 1), tiled across the screen. The renderer fills
// this with blue noise or an ordered (Bayer) matrix depending on the selected
// kernel, so one shader serves both. t0-t2 hold the video planes and upscaler
// inputs, so the thresholds live in t3.
Texture2D<float> ditherThresholds : register(t3);

// levels is (2^bitsPerComponent - 1) for the depth this frame is quantized to.
// For the video shaders it arrives in the tail of CSC_CONST_BUF.
float3 orderedDither(min16float3 color, float2 screenPos, float levels)
{
    uint width, height;
    ditherThresholds.GetDimensions(width, height);
    uint2 texel = uint2(screenPos) % uint2(width, height);

    // The thresholds already carry half a step of offset, which keeps the
    // pattern centered on the rounded value. Advancing the whole set by
    // ditherPhase and wrapping rotates it without disturbing its distribution,
    // so each frame stays a valid dither while the per-pixel threshold
    // decorrelates over time.
    float threshold = frac(ditherThresholds.Load(int3(texel, 0)) + ditherPhase);

    // Quantize at full precision. The result is already an exact output-depth
    // value, so the truncation further down the display pipeline is a no-op.
    return floor(saturate(float3(color)) * levels + threshold) / levels;
}

#define FINISH_VIDEO_SHADER(rgb, screenPos) \
    float4(orderedDither(rgb, screenPos, ditherLevels), 1.0f)

#else

#define VIDEO_SHADER_OUTPUT min16float4
#define FINISH_VIDEO_SHADER(rgb, screenPos) min16float4(rgb, 1.0)

#endif
