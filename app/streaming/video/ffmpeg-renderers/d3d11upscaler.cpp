#include "d3d11upscaler.h"

#include "path.h"

#include <SDL.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

uint32_t D3D11Upscaler::floatBits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

bool D3D11Upscaler::loadPixelShader(ID3D11Device* device, const char* name, ComPtr<ID3D11PixelShader>& shader)
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

bool D3D11Upscaler::createTarget(ID3D11Device* device, int width, int height,
                                 DXGI_FORMAT format, UINT bindFlags, Target& target)
{
    target = {};

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = format;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = bindFlags;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, &target.texture);
    if (FAILED(hr)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "ID3D11Device::CreateTexture2D() failed for a %dx%d upscaler target (format %d): %x",
                     width, height, format, hr);
        return false;
    }

    if (bindFlags & D3D11_BIND_RENDER_TARGET) {
        hr = device->CreateRenderTargetView(target.texture.Get(), nullptr, &target.rtv);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateRenderTargetView() failed for an upscaler target: %x",
                         hr);
            target = {};
            return false;
        }
    }

    if (bindFlags & D3D11_BIND_SHADER_RESOURCE) {
        hr = device->CreateShaderResourceView(target.texture.Get(), nullptr, &target.srv);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateShaderResourceView() failed for an upscaler target: %x",
                         hr);
            target = {};
            return false;
        }
    }

    if (bindFlags & D3D11_BIND_UNORDERED_ACCESS) {
        hr = device->CreateUnorderedAccessView(target.texture.Get(), nullptr, &target.uav);
        if (FAILED(hr)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "ID3D11Device::CreateUnorderedAccessView() failed for an upscaler target (format %d): %x",
                         format, hr);
            target = {};
            return false;
        }
    }

    target.width = width;
    target.height = height;
    return true;
}

bool D3D11Upscaler::createQuad(ID3D11Device* device, float uMax, float vMax)
{
    // Covers the whole viewport. Only the color conversion pass reads the
    // texture coordinates; the upscaling passes address pixels by position.
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
                     "ID3D11Device::CreateBuffer() failed for the upscaler quad: %x",
                     hr);
        return false;
    }

    m_QuadUMax = uMax;
    m_QuadVMax = vMax;
    return true;
}

bool D3D11Upscaler::configure(ID3D11Device* device, ID3D11DeviceContext* context,
                              int srcWidth, int srcHeight,
                              int dstX, int dstY, int dstWidth, int dstHeight,
                              float uMax, float vMax)
{
    // Only run when the output has more pixels than the stream, like the
    // Vulkan renderer. Otherwise the renderer's ordinary bilinear draw is used.
    const bool enlarged = srcWidth > 0 && srcHeight > 0 &&
                          (int64_t)dstWidth * dstHeight > (int64_t)srcWidth * srcHeight;
    if (!enlarged) {
        if (m_Active.exchange(false, std::memory_order_relaxed)) {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "%s inactive: %dx%d stream is not enlarged in a %dx%d area",
                        name(), srcWidth, srcHeight, dstWidth, dstHeight);
        }
        return true;
    }

    m_Active.store(false, std::memory_order_relaxed);

    if ((m_Source.width != srcWidth || m_Source.height != srcHeight) &&
            !createTarget(device, srcWidth, srcHeight, m_SourceFormat,
                          D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, m_Source)) {
        return false;
    }
    if ((!m_QuadVertexBuffer || m_QuadUMax != uMax || m_QuadVMax != vMax) &&
            !createQuad(device, uMax, vMax)) {
        return false;
    }

    m_SourceViewport = { 0, 0, (float)srcWidth, (float)srcHeight, 0, 1 };
    m_DestinationViewport = { (float)dstX, (float)dstY, (float)dstWidth, (float)dstHeight, 0, 1 };
    m_DstX = dstX;
    m_DstY = dstY;
    m_DstWidth = dstWidth;
    m_DstHeight = dstHeight;

    if (!configureOutput(device, context)) {
        return false;
    }

    m_Active.store(true, std::memory_order_relaxed);
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "%s active: %dx%d stream upscaled to %dx%d",
                name(), srcWidth, srcHeight, dstWidth, dstHeight);
    return true;
}

void D3D11Upscaler::beginSourcePass(ID3D11DeviceContext* context)
{
    UINT stride = sizeof(QuadVertex);
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, m_QuadVertexBuffer.GetAddressOf(), &stride, &offset);
    context->OMSetRenderTargets(1, m_Source.rtv.GetAddressOf(), nullptr);
    context->RSSetViewports(1, &m_SourceViewport);
}

void D3D11Upscaler::drawQuad(ID3D11DeviceContext* context)
{
    UINT stride = sizeof(QuadVertex);
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, m_QuadVertexBuffer.GetAddressOf(), &stride, &offset);
    context->DrawIndexed(6, 0, 0);
}
