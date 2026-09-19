VIDEO_SHADER_OUTPUT main(ShaderInput input) : SV_TARGET
{
    min16float3 yuv = swizzle(videoTex.Sample(theSampler, input.tex));

    // Subtract the YUV offset for limited vs full range
    yuv -= offsets;

    // Multiply by the conversion matrix for this colorspace
    yuv = mul(yuv, cscMatrix);

    return FINISH_VIDEO_SHADER(yuv, input.pos.xy);
}
