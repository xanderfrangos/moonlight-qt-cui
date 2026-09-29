// Debanding, ported from libplacebo's pl_shader_deband() (src/shaders/sampling.c,
// LGPL-2.1+) so the D3D11 renderer matches the Vulkan renderer's presets.
//
// Like libplacebo, it works on the raw Y'CbCr planes before color conversion,
// at each plane's own texel size: for every pass it averages four samples at
// a random offset and angle, and takes that average unless the pixel differs
// from it by more than the threshold (a real edge). Grain then covers what is
// left, fading out near black so dark scenes don't turn noisy.
//
// Defining DEBAND_INPUT selects the debanding variant of a video pixel shader.

#ifdef DEBAND_INPUT

// Rewritten every frame. Threshold, grain and neutral are already normalized
// to the texture's sample scale, as libplacebo does on the CPU.
cbuffer DEBAND_CONST_BUF : register(b3)
{
    // Y, Cb, Cr values that grain fades out towards
    float3 debandNeutral;
    uint debandIterations;
    // Last valid luma texcoord, so offsets don't reach into alignment padding
    float2 debandLumaMax;
    // In texels of the plane being debanded
    float debandRadius;
    float debandThreshold;
    float debandGrain;
    uint debandFrameIndex;
};

// pcg3d (http://jcgt.org/published/0009/03/02/), as libplacebo's sh_prng()
float3 debandRand(inout uint3 s)
{
    s = 1664525u * s + 1013904223u;
    s.x += s.y * s.z;
    s.y += s.z * s.x;
    s.z += s.x * s.y;
    s ^= s >> 16u;
    s.x += s.y * s.z;
    s.y += s.z * s.x;
    s.z += s.x * s.y;
    return float3(s) * (1.0f / 4294967295.0f);
}

// Seeded per screen pixel and per frame, so the pattern never sits still.
// libplacebo does the same with its frame index.
uint3 debandSeed(float2 screenPos)
{
    return uint3(uint2(screenPos), debandFrameIndex);
}

// Defines T NAME(float2 pos, float2 pt, T neutral, inout uint3 rng), which
// returns the debanded value of a plane sampled with FETCH(pos). pt is the
// size of one texel of that plane. RAND_TO_T narrows the float3 from
// debandRand() to T.
#define DEFINE_DEBAND(NAME, T, FETCH, RAND_TO_T)                            \
T NAME(float2 pos, float2 pt, T neutral, inout uint3 rng)                   \
{                                                                           \
    T res = FETCH(pos);                                                     \
                                                                            \
    [loop]                                                                  \
    for (uint i = 1; i <= debandIterations; i++) {                          \
        /* A random distance within this pass's radius, at a random angle */ \
        float2 d = debandRand(rng).xy * float2(i * debandRadius, 6.28318530718f); \
        d = d.x * float2(cos(d.y), sin(d.y));                               \
                                                                            \
        /* Average four samples at quarter-turn intervals around the pixel */ \
        T avg = FETCH(pos + pt * float2(+d.x, +d.y));                       \
        avg += FETCH(pos + pt * float2(-d.x, +d.y));                        \
        avg += FETCH(pos + pt * float2(-d.x, -d.y));                        \
        avg += FETCH(pos + pt * float2(+d.x, -d.y));                        \
        avg *= 0.25f;                                                       \
                                                                            \
        /* Keep the pixel where it differs by more than the threshold */    \
        T diff = abs(res - avg);                                            \
        res = diff > debandThreshold / i ? res : avg;                       \
    }                                                                       \
                                                                            \
    /* Add some random noise to smooth out residual differences, but avoid */ \
    /* adding grain near true black */                                      \
    if (debandGrain > 0.0f) {                                               \
        T strength = min(abs(res - neutral), debandGrain);                  \
        res += strength * (RAND_TO_T(debandRand(rng)) - 0.5f);              \
    }                                                                       \
                                                                            \
    return res;                                                             \
}

#define DEBAND_RAND_TO_FLOAT(v) (v).x
#define DEBAND_RAND_TO_FLOAT2(v) (v).xy
#define DEBAND_RAND_TO_FLOAT3(v) (v)

#endif
