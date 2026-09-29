#include "dithergrainhook.h"

#include <libplacebo/dither.h>

#include <cmath>
#include <vector>

// The grain math, appended once to libplacebo's final shader. The grain_*
// names are the variables and descriptor attached in run().
static const char* const k_GrainHeader = R"GLSL(
const float GRAIN_PANEL_GAMMA = 2.2;
const float GRAIN_PANEL_LEVELS = 1023.0;

// SMPTE ST 2084, normalized so 1.0 is 10000 nits
const float GRAIN_PQ_M1 = 0.1593017578125;
const float GRAIN_PQ_M2 = 78.84375;
const float GRAIN_PQ_C1 = 0.8359375;
const float GRAIN_PQ_C2 = 18.8515625;
const float GRAIN_PQ_C3 = 18.6875;

vec3 grain_pq_to_linear(vec3 pq)
{
    vec3 p = pow(pq, vec3(1.0 / GRAIN_PQ_M2));
    return pow(max(p - GRAIN_PQ_C1, 0.0) / (GRAIN_PQ_C2 - GRAIN_PQ_C3 * p), vec3(1.0 / GRAIN_PQ_M1));
}

vec3 grain_linear_to_pq(vec3 lin)
{
    vec3 p = pow(max(lin, 0.0), vec3(GRAIN_PQ_M1));
    return pow((GRAIN_PQ_C1 + GRAIN_PQ_C2 * p) / (1.0 + GRAIN_PQ_C3 * p), vec3(GRAIN_PQ_M2));
}

// PQ value of a display-gamma value in panel steps
vec3 grain_panel_to_pq(vec3 panel)
{
    return grain_linear_to_pq(pow(max(panel / GRAIN_PANEL_LEVELS, 0.0), vec3(GRAIN_PANEL_GAMMA)) /
                              grain_peak_scale);
}

vec3 grain_apply(vec3 color)
{
    // Half a tile away from the pixel, clear of libplacebo's own blue noise
    // dither, which comes from the same generator
    ivec2 size = textureSize(grain_noise, 0);
    ivec2 texel = (ivec2(gl_FragCoord.xy) + size / 2) % size;
    float noise = fract(texelFetch(grain_noise, texel, 0).x + grain_phase) * 2.0 - 1.0;

    vec3 rgb = clamp(color, 0.0, 1.0);
    vec3 amplitude;
    if (grain_pq != 0.0) {
        // Size the grain in the display's own steps, but add it symmetrically
        // in PQ, which is what the dithering reduces, so the average holds
        vec3 panel = pow(max(grain_pq_to_linear(rgb) * grain_peak_scale, 0.0),
                         vec3(1.0 / GRAIN_PANEL_GAMMA)) * GRAIN_PANEL_LEVELS;
        vec3 steps = mix(vec3(grain_high), vec3(grain_low),
                         lessThanEqual(panel, vec3(grain_low_below * GRAIN_PANEL_LEVELS)));
        steps = max(min(steps, min(panel, grain_panel_max - panel)), 0.0);
        amplitude = (grain_panel_to_pq(panel + steps) - grain_panel_to_pq(panel - steps)) * 0.5;
    } else {
        vec3 steps = mix(vec3(grain_high), vec3(grain_low),
                         lessThanEqual(rgb, vec3(grain_low_below)));
        amplitude = steps / grain_levels;
    }

    // Never past either end of the range. A clipped excursion would shift the
    // average, so the grain fades out toward black and white instead, and
    // black stays black. Channels with no amplitude are returned exactly.
    amplitude = max(min(amplitude, min(rgb, 1.0 - rgb)), 0.0);
    return color + noise * amplitude;
}
)GLSL";

std::unique_ptr<DitherGrainHook> DitherGrainHook::create(pl_gpu gpu, const DitherGrainPreset& preset,
                                                         bool temporal)
{
    // The same 64x64 blue noise the D3D11 renderer uses, with half a step of
    // offset so the noise is centered on zero
    static const int k_NoiseSize = 64;
    std::vector<float> noise(k_NoiseSize * k_NoiseSize);
    pl_generate_blue_noise(noise.data(), k_NoiseSize);
    const float halfStep = 0.5f / (float)noise.size();
    for (float& value : noise) {
        value += halfStep;
    }

    pl_tex_params texParams = {};
    texParams.w = k_NoiseSize;
    texParams.h = k_NoiseSize;
    texParams.format = pl_find_fmt(gpu, PL_FMT_FLOAT, 1, 32, 32, PL_FMT_CAP_SAMPLEABLE);
    texParams.sampleable = true;
    texParams.initial_data = noise.data();
    if (texParams.format == nullptr) {
        return nullptr;
    }

    std::unique_ptr<DitherGrainHook> grain(new DitherGrainHook());
    grain->m_Noise = pl_tex_create(gpu, &texParams);
    if (grain->m_Noise == nullptr) {
        return nullptr;
    }

    grain->m_Gpu = gpu;
    grain->m_Preset = preset;
    grain->m_Temporal = temporal;
    grain->m_Hook.stages = PL_HOOK_OUTPUT;
    grain->m_Hook.input = PL_HOOK_SIG_COLOR;
    grain->m_Hook.priv = grain.get();
    grain->m_Hook.hook = run;
    grain->m_Hook.signature = 0x4d4c4449544852ULL; // Arbitrary, unique to this hook
    return grain;
}

DitherGrainHook::~DitherGrainHook()
{
    pl_tex_destroy(m_Gpu, &m_Noise);
}

void DitherGrainHook::setTarget(const pl_frame& target)
{
    if (m_Temporal) {
        // The same golden ratio walk as the D3D11 thresholds
        m_Phase += 0.6180339887f;
        if (m_Phase >= 1.0f) {
            m_Phase -= 1.0f;
        }
    }

    // libplacebo only dithers targets below 16 bits. Anything deeper gets no
    // quantizer after the hook, so it gets no grain either.
    const int depth = target.repr.bits.color_depth;
    const bool quantized = depth > 0 && depth < 16;
    const bool pq = target.color.transfer == PL_COLOR_TRC_PQ;

    m_High = !quantized ? 0.0f : pq ? m_Preset.hdrHigh : m_Preset.sdrHigh;
    m_Low = pq ? m_Preset.hdrLow : m_Preset.sdrLow;
    m_LowBelow = pq ? m_Preset.hdrLowBelow : m_Preset.sdrLowBelow;
    m_Levels = quantized ? (float)((1 << depth) - 1) : 65535.0f;
    m_Pq = pq ? 1.0f : 0.0f;

    // libplacebo tone maps to the target's peak, so that's the brightest the
    // grain will see
    m_PeakScale = getDitherGrainPeakScale(target.color.hdr.max_luma);
    m_PanelMax = getDitherGrainPanelMax(m_PeakScale);
}

pl_hook_res DitherGrainHook::run(void* priv, const pl_hook_params* params)
{
    auto* self = static_cast<DitherGrainHook*>(priv);
    pl_hook_res res = {};

    // Leave libplacebo's shader untouched when there's nothing to add
    if (self->m_High <= 0.0f) {
        res.output = PL_HOOK_SIG_NONE;
        return res;
    }

    // pl_shader_custom() copies the values, so members are fine to point at
    const pl_shader_var vars[] = {
        { pl_var_float("grain_phase"), &self->m_Phase, true },
        { pl_var_float("grain_high"), &self->m_High, false },
        { pl_var_float("grain_low"), &self->m_Low, false },
        { pl_var_float("grain_low_below"), &self->m_LowBelow, false },
        { pl_var_float("grain_levels"), &self->m_Levels, false },
        { pl_var_float("grain_pq"), &self->m_Pq, false },
        { pl_var_float("grain_peak_scale"), &self->m_PeakScale, false },
        { pl_var_float("grain_panel_max"), &self->m_PanelMax, false },
    };

    pl_shader_desc noise = {};
    noise.desc.name = "grain_noise";
    noise.desc.type = PL_DESC_SAMPLED_TEX;
    noise.binding.object = self->m_Noise;
    noise.binding.sample_mode = PL_TEX_SAMPLE_NEAREST;
    noise.binding.address_mode = PL_TEX_ADDRESS_REPEAT;

    pl_custom_shader shader = {};
    shader.description = "blue noise dither grain";
    shader.header = k_GrainHeader;
    shader.body = "color.rgb = grain_apply(color.rgb);";
    shader.input = PL_SHADER_SIG_COLOR;
    shader.output = PL_SHADER_SIG_COLOR;
    shader.descriptors = &noise;
    shader.num_descriptors = 1;
    shader.variables = vars;
    shader.num_variables = (int)(sizeof(vars) / sizeof(vars[0]));

    // libplacebo requires a color hook to hand back a shader sized exactly to
    // the image. The final pass often hasn't fixed its size yet at this point,
    // so pin it to the image rect it gave us.
    int width, height;
    if (!pl_shader_output_size(params->sh, &width, &height)) {
        shader.output_w = (int)lroundf(pl_rect_w(params->rect));
        shader.output_h = (int)lroundf(pl_rect_h(params->rect));
    }

    if (!pl_shader_custom(params->sh, &shader)) {
        res.failed = true;
        return res;
    }

    res.output = PL_HOOK_SIG_COLOR;
    res.sh = params->sh;
    res.repr = params->repr;
    res.color = params->color;
    res.components = params->components;
    res.rect = params->rect;
    return res;
}
