// Copies an upscaler's destination-sized output into the destination
// rectangle of the back buffer, one texel per pixel. DITHER_OUTPUT selects the
// variant that quantizes for a display with fewer bits than the stream.

Texture2D<float4> upscaledTexture : register(t0);

struct ShaderInput
{
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

// b0 and b1 belong to the color conversion and dithering shaders
cbuffer COPY_CONST_BUF : register(b2)
{
    int2 dstOffset;
    float ditherLevels;
};

#include "d3d11_dither.hlsli"

float4 main(ShaderInput input) : SV_TARGET
{
    // The viewport starts at the destination rectangle, but SV_Position is
    // relative to the whole render target.
    float3 color = saturate(upscaledTexture.Load(int3(int2(input.pos.xy) - dstOffset, 0)).rgb);

#ifdef DITHER_OUTPUT
    return float4(orderedDither(min16float3(color), input.pos.xy, ditherLevels), 1.0f);
#else
    return float4(color, 1.0f);
#endif
}
