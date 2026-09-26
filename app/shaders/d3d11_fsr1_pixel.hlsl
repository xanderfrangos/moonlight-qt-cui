// AMD FidelityFX Super Resolution 1.0 for the D3D11 renderer, as two pixel
// shader passes over RGB that the color conversion shader already produced.
//
// APPLY_EASU selects the upscaling pass, which renders the whole destination
// rectangle into an intermediate texture of that size. APPLY_RCAS selects the
// sharpening pass, which reads that texture and renders into the back buffer
// with the viewport covering the destination rectangle.
//
// FSR_PQ=1 selects the HDR variants. FSR expects perceptual input, and PQ is
// converted to an approximate gamma 2.0 on the way into each pass and back on
// the way out, like FSR1_HDR.glsl does for the Vulkan renderer.

#define A_GPU 1
#define A_HLSL 1

#ifndef FSR_PQ
#define FSR_PQ 0
#endif

#include "enhancer/AMD/ffx_a.h"

Texture2D<float4> inputTexture : register(t0);
SamplerState theSampler : register(s0);

struct ShaderInput
{
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

// b0 and b1 belong to the color conversion and dithering shaders, and stay
// bound across frames, so FSR uses its own slot.
cbuffer FSR_CONST_BUF : register(b2)
{
    uint4 easuCon0;
    uint4 easuCon1;
    uint4 easuCon2;
    uint4 easuCon3;
    uint4 rcasCon;
    int2 dstOffset;
    float ditherLevels;
};

#if FSR_PQ
AF4 ToGamma2(AF4 a)
{
    a *= a;
    return a * a;
}

AF1 ToGamma2(AF1 a)
{
    a *= a;
    return a * a;
}

AF3 FromGamma2(AF3 a)
{
    return sqrt(sqrt(saturate(a)));
}
#else
#define ToGamma2(a) (a)
#define FromGamma2(a) (a)
#endif

#ifdef APPLY_EASU
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return ToGamma2(inputTexture.GatherRed(theSampler, p)); }
AF4 FsrEasuGF(AF2 p) { return ToGamma2(inputTexture.GatherGreen(theSampler, p)); }
AF4 FsrEasuBF(AF2 p) { return ToGamma2(inputTexture.GatherBlue(theSampler, p)); }
#endif

#ifdef APPLY_RCAS
#define FSR_RCAS_F 1
// Lessens sharpening in noisy areas, as the Vulkan renderer's shader does
#define FSR_RCAS_DENOISE 1
AF4 FsrRcasLoadF(ASU2 p) { return inputTexture.Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b)
{
    r = ToGamma2(r);
    g = ToGamma2(g);
    b = ToGamma2(b);
}
#endif

#include "enhancer/AMD/ffx_fsr1.h"

#include "d3d11_dither.hlsli"

float4 main(ShaderInput input) : SV_TARGET
{
    AF3 color;

#ifdef APPLY_EASU
    FsrEasuF(color, AU2(input.pos.xy), easuCon0, easuCon1, easuCon2, easuCon3);
#else
    // The viewport starts at the destination rectangle, but SV_Position is
    // relative to the whole render target.
    FsrRcasF(color.r, color.g, color.b, AU2(ASU2(input.pos.xy) - dstOffset), rcasCon);
#endif

    color = FromGamma2(color);

#ifdef DITHER_OUTPUT
    return float4(orderedDither(min16float3(color), input.pos.xy, ditherLevels), 1.0f);
#else
    return float4(color, 1.0f);
#endif
}
