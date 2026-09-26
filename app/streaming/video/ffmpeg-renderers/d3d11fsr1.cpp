#include "d3d11fsr1.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>

bool D3D11Fsr1Upscaler::initialize(ID3D11Device* device, bool tenBit, double sharpness)
{
    // Match the stream's precision so the intermediates don't add banding
    m_SourceFormat = tenBit ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;

    if (!loadPixelShader(device, "d3d11_fsr1_easu_pixel.fxc", m_EasuShader) ||
            !loadPixelShader(device, "d3d11_fsr1_easu_pq_pixel.fxc", m_EasuPqShader) ||
            !loadPixelShader(device, "d3d11_fsr1_rcas_pixel.fxc", m_RcasShader) ||
            !loadPixelShader(device, "d3d11_fsr1_rcas_pq_pixel.fxc", m_RcasPqShader) ||
            !loadPixelShader(device, "d3d11_fsr1_rcas_dither_pixel.fxc", m_RcasDitherShader)) {
        return false;
    }

    // FsrRcasCon() from ffx_fsr1.h, with the Vulkan renderer's mapping of the
    // 0-100 setting onto 2-0 stops. Only the 32-bit path is used, so the
    // packed half-precision value it also stores is left zero.
    const float stops = (float)((100.0 - std::clamp(sharpness, 0.0, 100.0)) / 50.0);
    m_Constants.rcasCon[0] = floatBits(std::exp2(-stops));
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

bool D3D11Fsr1Upscaler::configureOutput(ID3D11Device* device, ID3D11DeviceContext* context)
{
    if ((m_Easu.width != m_DstWidth || m_Easu.height != m_DstHeight) &&
            !createTarget(device, m_DstWidth, m_DstHeight, m_SourceFormat,
                          D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, m_Easu)) {
        return false;
    }

    // FsrEasuCon() from ffx_fsr1.h. The intermediate holds exactly the
    // stream, so the input viewport and the input texture are the same size.
    const float inW = (float)m_Source.width;
    const float inH = (float)m_Source.height;
    const float outW = (float)m_DstWidth;
    const float outH = (float)m_DstHeight;
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

    m_Constants.dstOffset[0] = m_DstX;
    m_Constants.dstOffset[1] = m_DstY;

    context->UpdateSubresource(m_ConstantBuffer.Get(), 0, nullptr, &m_Constants, 0, 0);
    return true;
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
    drawQuad(context);

    // RCAS: sharpen into the destination rectangle of the back buffer
    ID3D11PixelShader* rcasShader = pq ? m_RcasPqShader.Get() :
                                    dither ? m_RcasDitherShader.Get() :
                                             m_RcasShader.Get();
    context->OMSetRenderTargets(1, &target, nullptr);
    context->RSSetViewports(1, &m_DestinationViewport);
    context->PSSetShader(rcasShader, nullptr, 0);
    context->PSSetShaderResources(0, 1, m_Easu.srv.GetAddressOf());
    drawQuad(context);

    // Unbind the intermediate so the next frame can render into it
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->PSSetShaderResources(0, 1, &nullSrv);
    context->RSSetViewports(1, &fullViewport);
}
