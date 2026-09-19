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

VIDEO_SHADER_OUTPUT main(ShaderInput input) : SV_TARGET
{
    // Clamp the chrominance texcoords to avoid sampling the row of texels adjacent to the alignment padding
    min16float3 yuv = min16float3(luminancePlane.Sample(theSampler, input.tex),
                                  chrominancePlane.Sample(theSampler, min(input.tex + chromaOffset, chromaTexMax.rg)));

    // Subtract the YUV offset for limited vs full range
    yuv -= offsets;

    // Multiply by the conversion matrix for this colorspace
    yuv = mul(yuv, cscMatrix);

    return FINISH_VIDEO_SHADER(yuv, input.pos.xy);
}
