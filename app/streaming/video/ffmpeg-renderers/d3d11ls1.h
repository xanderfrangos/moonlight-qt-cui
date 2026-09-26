#pragma once

#include "d3d11upscaler.h"

#include <QString>

#include <array>

// Lossless Scaling's LS1 upscaler, run from the user's own Steam-installed
// Lossless.dll. Lossless Scaling is a D3D11 application, so its compute
// shaders are loaded as they are, with no translation. Nothing from the DLL is
// stored by Moonlight.
//
// Three compute stages analyze the stream-sized RGB into a feature texture at
// twice the stream size, a fourth reconstructs the destination-sized image,
// and a pixel shader copies that into the back buffer. LS1 is SDR only.
class D3D11Ls1Upscaler : public D3D11Upscaler
{
public:
    // Lossless Scaling in any Steam library. Empty if it can't be found.
    static QString findLosslessScalingDll();

    // Variant 0-4 selects the model's sharpness
    bool initialize(ID3D11Device* device, const QString& dllPath, int variant,
                    bool tenBit, QString* error);

    const char* name() const override { return "LS1"; }
    bool handlesPq() const override { return false; }
    void upscale(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
                 bool pq, bool dither, float ditherLevels,
                 const D3D11_VIEWPORT& fullViewport) override;

protected:
    bool configureOutput(ID3D11Device* device, ID3D11DeviceContext* context) override;

private:
    // The cb layout all four stages share
    struct Parameters {
        uint32_t sourceWidth, sourceHeight, capturedWidth, capturedHeight;
        uint32_t sourceOffsetX, sourceOffsetY, gammaPreprocess, reserved;
        uint32_t outputWidth, outputHeight, outputOffsetX, outputOffsetY;
    };
    static_assert(sizeof(Parameters) == 48, "LS1 parameter layout changed");

    // Mirrors COPY_CONST_BUF in d3d11_upscale_copy_pixel.hlsl
    struct CopyConstBuf {
        int32_t dstOffset[2];
        float ditherLevels;
        float padding;
    };
    static_assert(sizeof(CopyConstBuf) % 16 == 0, "Constant buffer sizes must be a multiple of 16");

    void dispatch(ID3D11DeviceContext* context, int stage,
                  ID3D11ShaderResourceView* input0, ID3D11ShaderResourceView* input1,
                  ID3D11UnorderedAccessView* output, int width, int height);

    std::array<Microsoft::WRL::ComPtr<ID3D11ComputeShader>, 4> m_Stages;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_Sampler;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_ParameterBuffer;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_CopyShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_CopyDitherShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_CopyBuffer;

    CopyConstBuf m_Copy = {};
    Target m_IntermediateA;
    Target m_IntermediateB;
    Target m_Feature;
    Target m_Output;
};
