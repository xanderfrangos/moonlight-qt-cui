#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>

// AMD FidelityFX Super Resolution 1.0 for the D3D11 renderer. When the video
// is drawn larger than the stream, the renderer converts the frame to RGB at
// stream size in an intermediate texture, and this class then upscales it with
// EASU into a second texture and sharpens it with RCAS into the back buffer.
//
// All passes are pixel shaders drawn with the renderer's vertex shader, input
// layout, index buffer and sampler, which stay bound. The shaders take their
// constants from b2 because b0 and b1 belong to the color conversion shaders.
class D3D11Fsr1Upscaler
{
public:
    // Loads the shaders. Sharpness is the 0-100 RCAS setting.
    bool initialize(ID3D11Device* device, bool tenBit, double sharpness);

    // Called whenever the frame size or the window size changes. The
    // destination rectangle is in render target pixels, measured from the top
    // left. uMax and vMax exclude the decoder's alignment padding. Returns
    // false only if resources couldn't be created, which also leaves the
    // upscaler inactive.
    bool configure(ID3D11Device* device, ID3D11DeviceContext* context,
                   int srcWidth, int srcHeight,
                   int dstX, int dstY, int dstWidth, int dstHeight,
                   float uMax, float vMax);

    // True when the last configure() found the video enlarged. Safe to call
    // from any thread.
    bool active() const { return m_Active.load(std::memory_order_relaxed); }

    // Binds the stream-sized intermediate as the render target, for the color
    // conversion draw.
    void beginSourcePass(ID3D11DeviceContext* context);

    // Runs EASU and RCAS, leaving the result in the destination rectangle of
    // target. Restores target and fullViewport for the overlays.
    void upscale(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
                 bool pq, bool dither, float ditherLevels,
                 const D3D11_VIEWPORT& fullViewport);

private:
    struct Target {
        int width = 0;
        int height = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    };

    bool createTarget(ID3D11Device* device, int width, int height, Target& target);
    bool createQuad(ID3D11Device* device, float uMax, float vMax);

    // Mirrors FSR_CONST_BUF in d3d11_fsr1_pixel.hlsl
    struct ConstBuf {
        uint32_t easuCon[4][4];
        uint32_t rcasCon[4];
        int32_t dstOffset[2];
        float ditherLevels;
        float padding;
    };
    static_assert(sizeof(ConstBuf) % 16 == 0, "Constant buffer sizes must be a multiple of 16");

    DXGI_FORMAT m_Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    float m_RcasStops = 0.0f;

    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_EasuShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_EasuPqShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_RcasShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_RcasPqShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_RcasDitherShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_ConstantBuffer;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_QuadVertexBuffer;
    float m_QuadUMax = 0.0f;
    float m_QuadVMax = 0.0f;

    ConstBuf m_Constants = {};
    Target m_Source;
    Target m_Easu;
    D3D11_VIEWPORT m_SourceViewport = {};
    D3D11_VIEWPORT m_DestinationViewport = {};
    std::atomic<bool> m_Active{false};
};
