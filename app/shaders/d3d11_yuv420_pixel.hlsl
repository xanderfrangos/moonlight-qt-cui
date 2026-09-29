Texture2D<min16float> luminancePlane : register(t0);
Texture2D<min16float2> chrominancePlane : register(t1);
SamplerState theSampler : register(s0);

struct ShaderInput
{
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

cbuffer CSC_CONST_BUF : register(b0)
{
    min16float3x3 cscMatrix;
    min16float3 offsets;
    min16float2 chromaOffset;
    min16float2 chromaTexMax;
    float ditherLevels;
};

#include "d3d11_dither.hlsli"
#include "d3d11_deband.hlsli"

#ifdef DEBAND_INPUT
#define FETCH_LUMA(p) float(luminancePlane.SampleLevel(theSampler, min(p, debandLumaMax), 0))
#define FETCH_CHROMA(p) float2(chrominancePlane.SampleLevel(theSampler, min(p, float2(chromaTexMax)), 0))
DEFINE_DEBAND(debandLuma, float, FETCH_LUMA, DEBAND_RAND_TO_FLOAT)
DEFINE_DEBAND(debandChroma, float2, FETCH_CHROMA, DEBAND_RAND_TO_FLOAT2)
#endif

VIDEO_SHADER_OUTPUT main(ShaderInput input) : SV_TARGET
{
#ifdef DEBAND_INPUT
    // Each plane is debanded at its own texel size, luma then chroma, drawing
    // from one random sequence so the two don't share offsets or grain.
    uint lumaWidth, lumaHeight, chromaWidth, chromaHeight;
    luminancePlane.GetDimensions(lumaWidth, lumaHeight);
    chrominancePlane.GetDimensions(chromaWidth, chromaHeight);

    uint3 rng = debandSeed(input.pos.xy);
    float y = debandLuma(input.tex, 1.0f / float2(lumaWidth, lumaHeight),
                         debandNeutral.x, rng);
    float2 cbcr = debandChroma(min(input.tex + chromaOffset, chromaTexMax.rg),
                               1.0f / float2(chromaWidth, chromaHeight),
                               debandNeutral.yz, rng);
    min16float3 yuv = min16float3(y, cbcr);
#else
    // Clamp the chrominance texcoords to avoid sampling the row of texels adjacent to the alignment padding
    min16float3 yuv = min16float3(luminancePlane.Sample(theSampler, input.tex),
                                  chrominancePlane.Sample(theSampler, min(input.tex + chromaOffset, chromaTexMax.rg)));
#endif

    // Subtract the YUV offset for limited vs full range
    yuv -= offsets;

    // Multiply by the conversion matrix for this colorspace
    yuv = mul(yuv, cscMatrix);

    return FINISH_VIDEO_SHADER(yuv, input.pos.xy);
}
