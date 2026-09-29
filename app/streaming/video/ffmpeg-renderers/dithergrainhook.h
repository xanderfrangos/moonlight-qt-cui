#pragma once

#include "streaming/video/dithergrain.h"

#include <libplacebo/renderer.h>
#include <libplacebo/shaders/custom.h>

#include <memory>

// Blue noise grain (dithergrain.h) as a libplacebo OUTPUT hook. The math
// matches addDitherGrain() in d3d11_dither.hlsli.
//
// libplacebo runs OUTPUT hooks after encoding to the target and before its
// own dithering. This hook takes the pre-sampled color signature, so its code
// is appended to that same final shader: the grain adds no intermediate
// texture, and libplacebo's dithering still quantizes a full-precision value
// exactly as it would without grain. An mpv-style user shader can't do that;
// it would force a 16-bit float texture in between, which rounds the image
// before the quantizer ever sees it.
class DitherGrainHook
{
public:
    // Returns null if the GPU can't hold the noise texture
    static std::unique_ptr<DitherGrainHook> create(pl_gpu gpu, const DitherGrainPreset& preset,
                                                   bool temporal);
    ~DitherGrainHook();

    const pl_hook* hook() const { return &m_Hook; }

    // Sizes the grain for the target the next render draws into, and
    // advances the temporal phase. Renders must not overlap with this.
    void setTarget(const pl_frame& target);

private:
    DitherGrainHook() = default;
    static pl_hook_res run(void* priv, const pl_hook_params* params);

    pl_gpu m_Gpu = nullptr;
    pl_tex m_Noise = nullptr;
    pl_hook m_Hook = {};
    DitherGrainPreset m_Preset = {};
    bool m_Temporal = false;

    // Shader inputs for the current target. m_High is zero when libplacebo
    // won't quantize the target, and then the hook adds nothing.
    float m_Phase = 0.0f;
    float m_High = 0.0f;
    float m_Low = 0.0f;
    float m_LowBelow = 0.0f;
    float m_Levels = 255.0f;
    float m_Pq = 0.0f;
    float m_PeakScale = 10.0f;
    float m_PanelMax = 1023.0f;
};
