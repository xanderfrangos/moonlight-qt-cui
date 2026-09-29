#ifdef DEBAND_INPUT
// Swizzled to Y'CbCr order before debanding, so the neutral values line up
#define FETCH_444(p) float3(swizzle(videoTex.SampleLevel(theSampler, min(p, debandLumaMax), 0)))
DEFINE_DEBAND(deband444, float3, FETCH_444, DEBAND_RAND_TO_FLOAT3)
#endif

VIDEO_SHADER_OUTPUT main(ShaderInput input) : SV_TARGET
{
#ifdef DEBAND_INPUT
    uint width, height;
    videoTex.GetDimensions(width, height);
    uint3 rng = debandSeed(input.pos.xy);
    min16float3 yuv = min16float3(deband444(input.tex, 1.0f / float2(width, height),
                                            debandNeutral, rng));
#else
    min16float3 yuv = swizzle(videoTex.Sample(theSampler, input.tex));
#endif

    // Subtract the YUV offset for limited vs full range
    yuv -= offsets;

    // Multiply by the conversion matrix for this colorspace
    yuv = mul(yuv, cscMatrix);

    return FINISH_VIDEO_SHADER(yuv, input.pos.xy);
}
