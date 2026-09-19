// Ordered dithering for displays that have fewer bits per component than the
// stream carries. Without it, the 10-bit video we hand to the swapchain is
// truncated somewhere on the way to the panel, which shows up as banding in
// gradients (skies, fog, dark scenes).
//
// Defining DITHER_OUTPUT selects the dithering variant of a video pixel
// shader. The shaders are otherwise identical, so enabling dithering never
// changes the color conversion itself, only the final quantization.

#ifdef DITHER_OUTPUT

#define VIDEO_SHADER_OUTPUT float4

// ditherLevels is (2^bitsPerComponent - 1) for the display we're rendering to.
// It arrives in the tail of CSC_CONST_BUF.
float3 orderedDither(min16float3 color, float2 screenPos, float levels)
{
    uint x = ((uint)screenPos.x) & 7u;
    uint y = ((uint)screenPos.y) & 7u;
    uint xy = x ^ y;

    // Interleave the bits of (x ^ y) and y, most significant bit first. This
    // builds the classic recursive 8x8 ordered dither matrix without a lookup.
    uint index = ((xy         & 1u) << 5) |
                 ((y          & 1u) << 4) |
                 (((xy >> 1u) & 1u) << 3) |
                 (((y  >> 1u) & 1u) << 2) |
                 (((xy >> 2u) & 1u) << 1) |
                  ((y  >> 2u) & 1u);

    // Half a step of offset keeps the pattern centered on the rounded value
    float threshold = ((float)index + 0.5f) / 64.0f;

    // Quantize at full precision. The result is already an exact display-depth
    // value, so the truncation further down the display pipeline is a no-op.
    return floor(saturate(float3(color)) * levels + threshold) / levels;
}

#define FINISH_VIDEO_SHADER(rgb, screenPos) \
    float4(orderedDither(rgb, screenPos, ditherLevels), 1.0f)

#else

#define VIDEO_SHADER_OUTPUT min16float4
#define FINISH_VIDEO_SHADER(rgb, screenPos) min16float4(rgb, 1.0)

#endif
