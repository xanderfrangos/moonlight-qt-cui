// Two-pass downscaling with normalized, ratio-aware weights prepared on resize.
// Cubic B-spline, Mitchell and Lanczos3 share the same shader and weight layout.
Texture2D<float4> sourceTexture : register(t0);
// Store explicit source indices so CPU/GPU rounding cannot shift the footprint.
Texture2D<float2> filterWeights : register(t1);

struct ShaderInput
{
    float4 pos : SV_POSITION;
    float2 tex : TEXCOORD0;
};

cbuffer DOWNSCALE_CONST_BUF : register(b2)
{
    int2 destinationOffset;
    int vertical;
    int transfer;
    float ditherLevels;
    int taps;
    int2 padding;
};

#include "d3d11_dither.hlsli"

float decodeTransfer(float value)
{
    float x = saturate(value);
    // Video BT.709 uses the display EOTF (BT.1886), not the camera OETF.
    if (transfer == 1) return pow(x, 2.4);
    if (transfer == 2) return x <= 0.04045 ? x / 12.92 : pow((x + 0.055) / 1.055, 2.4);
    if (transfer == 3) return pow(x, 2.8);
    if (transfer == 4) return x;
    if (transfer == 5) {
        float p = pow(x, 1.0 / 78.84375);
        return pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p), 1.0 / 0.1593017578125);
    }
    if (transfer == 6) return x <= 0.5 ? x*x / 3.0 : (exp((x - 0.55991073) / 0.17883277) + 0.28466892) / 12.0;
    return pow(x, 2.2);
}

float encodeTransfer(float value)
{
    float x = saturate(value);
    if (transfer == 1) return pow(x, 1.0 / 2.4);
    if (transfer == 2) return x <= 0.0031308 ? 12.92*x : 1.055*pow(x, 1.0 / 2.4) - 0.055;
    if (transfer == 3) return pow(x, 1.0 / 2.8);
    if (transfer == 4) return x;
    if (transfer == 5) {
        float p = pow(x, 0.1593017578125);
        return pow((0.8359375 + 18.8515625*p) / (1.0 + 18.6875*p), 78.84375);
    }
    if (transfer == 6) return x <= 1.0 / 12.0 ? sqrt(3.0*x) : 0.17883277*log(12.0*x - 0.28466892) + 0.55991073;
    return pow(x, 1.0 / 2.2);
}

float4 main(ShaderInput input) : SV_TARGET
{
    int2 pixel = int2(input.pos.xy) - destinationOffset;
    int axisPixel = vertical ? pixel.y : pixel.x;
    float3 color = 0.0;
    [loop]
    for (int tap = 0; tap < taps; ++tap) {
        float2 coefficient = filterWeights.Load(int3(tap, axisPixel, 0));
        if (coefficient.x != 0.0) {
            int index = int(coefficient.y);
            int2 coord = vertical ? int2(pixel.x, index) : int2(index, pixel.y);
            float3 sample = sourceTexture.Load(int3(coord, 0)).rgb;
            if (!vertical) sample = float3(decodeTransfer(sample.r), decodeTransfer(sample.g), decodeTransfer(sample.b));
            color += coefficient.x * sample;
        }
    }
    // Retain signed lobes between passes. Clamp only the completed reduction.
    if (!vertical) return float4(color, 1.0);
    color = float3(encodeTransfer(color.r), encodeTransfer(color.g), encodeTransfer(color.b));
#ifdef DITHER_OUTPUT
    color = orderedDither(min16float3(color), input.pos.xy, ditherLevels);
#endif
    return float4(color, 1.0);
}
