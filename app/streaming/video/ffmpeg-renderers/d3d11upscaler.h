#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>

// Base for the D3D11 renderer's optional upscalers. When the video is drawn
// larger than the stream, the renderer converts the frame to RGB at stream
// size in an intermediate texture owned by this class, and the subclass then
// upscales that into the destination rectangle of the back buffer.
//
// The passes are drawn with the renderer's vertex shader, input layout, index
// buffer and pixel sampler, which stay bound. Pixel shader constants go in b2
// because b0 and b1 belong to the color conversion shaders.
class D3D11Upscaler
{
public:
    virtual ~D3D11Upscaler() = default;

    // Short name for logs and the stream info overlay
    virtual const char* name() const = 0;

    // Whether upscale() can take PQ frames. Others are drawn directly.
    virtual bool handlesPq() const = 0;

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

    // Upscales the intermediate into the destination rectangle of target, then
    // restores target and fullViewport for the overlays. Dithering applies to
    // SDR output only.
    virtual void upscale(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
                         bool pq, bool dither, float ditherLevels,
                         const D3D11_VIEWPORT& fullViewport) = 0;

protected:
    struct Target {
        int width = 0;
        int height = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
        Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav;
    };

    struct QuadVertex {
        float x, y;
        float tu, tv;
    };

    // Creates the views that bindFlags asks for
    static bool createTarget(ID3D11Device* device, int width, int height,
                             DXGI_FORMAT format, UINT bindFlags, Target& target);
    static bool loadPixelShader(ID3D11Device* device, const char* name,
                                Microsoft::WRL::ComPtr<ID3D11PixelShader>& shader);
    static uint32_t floatBits(float value);

    // Sizes the subclass's own resources for the current source and destination
    virtual bool configureOutput(ID3D11Device* device, ID3D11DeviceContext* context) = 0;

    // Draws a quad covering the current viewport
    void drawQuad(ID3D11DeviceContext* context);

    DXGI_FORMAT m_SourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    Target m_Source;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_QuadVertexBuffer;
    D3D11_VIEWPORT m_SourceViewport = {};
    D3D11_VIEWPORT m_DestinationViewport = {};
    int m_DstX = 0;
    int m_DstY = 0;
    int m_DstWidth = 0;
    int m_DstHeight = 0;

private:
    bool createQuad(ID3D11Device* device, float uMax, float vMax);

    float m_QuadUMax = 0.0f;
    float m_QuadVMax = 0.0f;
    std::atomic<bool> m_Active{false};
};
