#pragma once

#include "d3d11upscaler.h"

// AMD FidelityFX Super Resolution 1.0: EASU upscales the stream-sized RGB
// into a destination-sized texture, then RCAS sharpens it into the back buffer.
class D3D11Fsr1Upscaler : public D3D11Upscaler
{
public:
    // Loads the shaders. Sharpness is the 0-100 RCAS setting.
    bool initialize(ID3D11Device* device, bool tenBit, double sharpness);

    const char* name() const override { return "FSR"; }
    bool handlesPq() const override { return true; }
    void upscale(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
                 bool pq, bool dither, float ditherLevels,
                 const D3D11_VIEWPORT& fullViewport) override;

protected:
    bool configureOutput(ID3D11Device* device, ID3D11DeviceContext* context) override;

private:
    // Mirrors FSR_CONST_BUF in d3d11_fsr1_pixel.hlsl
    struct ConstBuf {
        uint32_t easuCon[4][4];
        uint32_t rcasCon[4];
        int32_t dstOffset[2];
        float ditherLevels;
        float padding;
    };
    static_assert(sizeof(ConstBuf) % 16 == 0, "Constant buffer sizes must be a multiple of 16");

    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_EasuShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_EasuPqShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_RcasShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_RcasPqShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_RcasDitherShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_ConstantBuffer;

    ConstBuf m_Constants = {};
    Target m_Easu;
};
