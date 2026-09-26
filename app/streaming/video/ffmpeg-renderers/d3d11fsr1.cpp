#include "d3d11fsr1.h"

#include "path.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

struct QuadVertex
{
    float x, y;
    float tu, tv;
};

uint32_t floatBits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool loadPixelShader(ID3D11Device* device, const char* name, ComPtr<ID3D11PixelShader>& shader)
{
    QByteArray bytecode = Path::readDataFile(name);
    HRESULT hr = device->CreatePixelShader(bytecode.constData(), bytecode.length(), nullptr, &shader);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::CreatePixelShader() failed for %s: %x",
                     name, hr);
        return false;
    }
    return true;
}

}

bool D3D11Fsr1Upscaler::initialize(ID3D11Device* device, bool tenBit, double sharpness)
{
    // Match the stream's precision so the intermediates don't add banding
    m_Format = tenBit ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;

    // Same mapping as the Vulkan renderer: 0-100 maps to 2-0 RCAS stops
    m_RcasStops = (float)((100.0 - std::clamp(sharpness, 0.0, 100.0)) / 50.0);

    if (!loadPixelShader(device, "d3d11_fsr1_easu_pixel.fxc", m_EasuShader) ||
            !loadPixelShader(device, "d3d11_fsr1_easu_pq_pixel.fxc", m_EasuPqShader) ||
            !loadPixelShader(device, "d3d11_fsr1_rcas_pixel.fxc", m_RcasShader) ||
            !loadPixelShader(device, "d3d11_fsr1_rcas_pq_pixel.fxc", m_RcasPqShader) ||
            !loadPixelShader(device, "d3d11_fsr1_rcas_dither_pixel.fxc", m_RcasDitherShader)) {
        return false;
    }

    // FsrRcasCon() from ffx_fsr1.h. Only the 32-bit path is used, so the
    // packed half-precision value it also stores is left zero.
    m_Constants.rcasCon[0] = floatBits(std::exp2(-m_RcasStops));
    m_Constants.ditherLevels = 255.0f;

    D3D11_BUFFER_DESC constDesc = {};
    constDesc.ByteWidth = sizeof(ConstBuf);
    constDesc.Usage = D3D11_USAGE_DEFAULT;
    constDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;

    D3D11_SUBRESOURCE_DATA constData = {};
    constData.pSysMem = &m_Constants;

    HRESULT hr = device->CreateBuffer(&constDesc, &constData, &m_ConstantBuffer);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::CreateBuffer() failed for the FSR1 constants: %x",
                     hr);
        return false;
    }

    return true;
}

bool D3D11Fsr1Upscaler::createTarget(ID3D11Device* device, int width, int height, Target& target)
{
    target = {};

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = m_Format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &target.texture);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::CreateTexture2D() failed for a %dx%d FSR1 target: %x",
                     width, height, hr);
        return false;
    }

    hr = device->CreateRenderTargetView(target.texture.Get(), nullptr, &target.rtv);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::CreateRenderTargetView() failed for an FSR1 target: %x",
                     hr);
        target = {};
        return false;
    }

    hr = device->CreateShaderResourceView(target.texture.Get(), nullptr, &target.srv);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::CreateShaderResourceView() failed for an FSR1 target: %x",
                     hr);
        target = {};
        return false;
    }

    target.width = width;
    target.height = height;
    return true;
}

bool D3D11Fsr1Upscaler::createQuad(ID3D11Device* device, float uMax, float vMax)
{
    // Covers the whole viewport. Only the color conversion pass reads the
    // texture coordinates; the FSR passes address pixels by position.
    QuadVertex verts[] =
    {
        {-1.0f, -1.0f, 0, vMax},
        {-1.0f, 1.0f, 0, 0},
        {1.0f, -1.0f, uMax, vMax},
        {1.0f, 1.0f, uMax, 0},
    };

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.ByteWidth = sizeof(verts);
    vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vbDesc.StructureByteStride = sizeof(QuadVertex);

    D3D11_SUBRESOURCE_DATA vbData = {};
    vbData.pSysMem = verts;

    m_QuadVertexBuffer.Reset();
    HRESULT hr = device->CreateBuffer(&vbDesc, &vbData, &m_QuadVertexBuffer);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::CreateBuffer() failed for the FSR1 quad: %x",
                     hr);
        return false;
    }

    m_QuadUMax = uMax;
    m_QuadVMax = vMax;
    return true;
}

bool D3D11Fsr1Upscaler::configure(ID3D11Device* device, ID3D11DeviceContext* context,
                                  int srcWidth, int srcHeight,
                                  int dstX, int dstY, int dstWidth, int dstHeight,
                                  float uMax, float vMax)
{
    // Like the Vulkan shaders, only run when the output has more pixels than
    // the stream. Otherwise the renderer's ordinary bilinear draw is used.
    const bool enlarged = srcWidth > 0 && srcHeight > 0 &&
                          (int64_t)dstWidth * dstHeight > (int64_t)srcWidth * srcHeight;
    if (!enlarged) {
        if (m_Active.exchange(false, std::memory_order_relaxed)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "FSR1 inactive: %dx%d stream is not enlarged in a %dx%d area",
                        srcWidth, srcHeight, dstWidth, dstHeight);
        }
        return true;
    }

    m_Active.store(false, std::memory_order_relaxed);

    if ((m_Source.width != srcWidth || m_Source.height != srcHeight) &&
            !createTarget(device, srcWidth, srcHeight, m_Source)) {
        return false;
    }
    if ((m_Easu.width != dstWidth || m_Easu.height != dstHeight) &&
            !createTarget(device, dstWidth, dstHeight, m_Easu)) {
        return false;
    }
    if ((!m_QuadVertexBuffer || m_QuadUMax != uMax || m_QuadVMax != vMax) &&
            !createQuad(device, uMax, vMax)) {
        return false;
    }

    m_SourceViewport = { 0, 0, (float)srcWidth, (float)srcHeight, 0, 1 };
    m_DestinationViewport = { (float)dstX, (float)dstY, (float)dstWidth, (float)dstHeight, 0, 1 };

    // FsrEasuCon() from ffx_fsr1.h. The intermediate holds exactly the
    // stream, so the input viewport and the input texture are the same size.
    const float inW = (float)srcWidth;
    const float inH = (float)srcHeight;
    const float outW = (float)dstWidth;
    const float outH = (float)dstHeight;
    auto& con = m_Constants.easuCon;
    con[0][0] = floatBits(inW / outW);
    con[0][1] = floatBits(inH / outH);
    con[0][2] = floatBits(0.5f * inW / outW - 0.5f);
    con[0][3] = floatBits(0.5f * inH / outH - 0.5f);
    con[1][0] = floatBits(1.0f / inW);
    con[1][1] = floatBits(1.0f / inH);
    con[1][2] = floatBits(1.0f / inW);
    con[1][3] = floatBits(-1.0f / inH);
    con[2][0] = floatBits(-1.0f / inW);
    con[2][1] = floatBits(2.0f / inH);
    con[2][2] = floatBits(1.0f / inW);
    con[2][3] = floatBits(2.0f / inH);
    con[3][0] = floatBits(0.0f);
    con[3][1] = floatBits(4.0f / inH);
    con[3][2] = 0;
    con[3][3] = 0;

    m_Constants.dstOffset[0] = dstX;
    m_Constants.dstOffset[1] = dstY;

    context->UpdateSubresource(m_ConstantBuffer.Get(), 0, nullptr, &m_Constants, 0, 0);

    m_Active.store(true, std::memory_order_relaxed);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "FSR1 active: %dx%d stream upscaled to %dx%d",
                srcWidth, srcHeight, dstWidth, dstHeight);
    return true;
}

void D3D11Fsr1Upscaler::beginSourcePass(ID3D11DeviceContext* context)
{
    UINT stride = sizeof(QuadVertex);
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, m_QuadVertexBuffer.GetAddressOf(), &stride, &offset);
    context->OMSetRenderTargets(1, m_Source.rtv.GetAddressOf(), nullptr);
    context->RSSetViewports(1, &m_SourceViewport);
}

void D3D11Fsr1Upscaler::upscale(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
                                bool pq, bool dither, float ditherLevels,
                                const D3D11_VIEWPORT& fullViewport)
{
    // The display's depth can change while streaming
    if (dither && ditherLevels != m_Constants.ditherLevels) {
        m_Constants.ditherLevels = ditherLevels;
        context->UpdateSubresource(m_ConstantBuffer.Get(), 0, nullptr, &m_Constants, 0, 0);
    }

    context->PSSetConstantBuffers(2, 1, m_ConstantBuffer.GetAddressOf());

    // EASU: stream-sized RGB to the destination size. Binding the new render
    // target first releases the source texture's output binding.
    const D3D11_VIEWPORT easuViewport = { 0, 0, (float)m_Easu.width, (float)m_Easu.height, 0, 1 };
    context->OMSetRenderTargets(1, m_Easu.rtv.GetAddressOf(), nullptr);
    context->RSSetViewports(1, &easuViewport);
    context->PSSetShader(pq ? m_EasuPqShader.Get() : m_EasuShader.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, m_Source.srv.GetAddressOf());
    context->DrawIndexed(6, 0, 0);

    // RCAS: sharpen into the destination rectangle of the back buffer
    ID3D11PixelShader* rcasShader = pq ? m_RcasPqShader.Get() :
                                    dither ? m_RcasDitherShader.Get() :
                                             m_RcasShader.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    context->RSSetViewports(1, &m_DestinationViewport);
    context->PSSetShader(rcasShader, nullptr, 0);
    context->PSSetShaderResources(0, 1, m_Easu.srv.GetAddressOf());
    context->DrawIndexed(6, 0, 0);

    // Unbind the intermediate so the next frame can render into it
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->PSSetShaderResources(0, 1, &nullSrv);
    context->RSSetViewports(1, &fullViewport);
}
