#include "d3d11downscaler.h"

#include "settings/streamingpreferences.h"

extern "C" {
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
double kernel(int filter, double distance)
{
    const double x = std::abs(distance);
    if (filter == StreamingPreferences::DF_LANCZOS) {
        if (x >= 3.0) return 0.0;
        if (x < 1e-12) return 1.0;
        constexpr double pi = 3.14159265358979323846;
        return std::sin(pi * x) * std::sin(pi * x / 3.0) / (pi * pi * x * x / 3.0);
    }
    // Match libplacebo: B-spline (1,0) or Mitchell-Netravali (1/3,1/3).
    const double b = filter == StreamingPreferences::DF_MITCHELL ? 1.0 / 3.0 : 1.0;
    const double c = filter == StreamingPreferences::DF_MITCHELL ? 1.0 / 3.0 : 0.0;
    if (x < 1.0) {
        return ((12 - 9*b - 6*c)*x*x*x + (-18 + 12*b + 6*c)*x*x + 6 - 2*b) / 6;
    }
    if (x < 2.0) {
        return ((-b - 6*c)*x*x*x + (6*b + 30*c)*x*x + (-12*b - 48*c)*x + 8*b + 24*c) / 6;
    }
    return 0.0;
}
}

const char* D3D11Downscaler::name() const
{
    switch (m_Filter) {
    case StreamingPreferences::DF_MITCHELL: return "Mitchell downscaling";
    case StreamingPreferences::DF_LANCZOS: return "Lanczos downscaling";
    default: return "Bicubic downscaling";
    }
}

bool D3D11Downscaler::initialize(ID3D11Device* device, int filter)
{
    if (filter < StreamingPreferences::DF_BICUBIC || filter > StreamingPreferences::DF_LANCZOS) {
        return false;
    }
    m_Filter = filter;
    // The color conversion must retain fractional values before filtering.
    m_SourceFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (!loadPixelShader(device, "d3d11_downscale_pixel.fxc", m_FilterShader) ||
            !loadPixelShader(device, "d3d11_downscale_dither_pixel.fxc", m_FilterDitherShader)) {
        return false;
    }
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = sizeof(ConstBuf);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(device->CreateBuffer(&desc, nullptr, &m_ConstantBuffer));
}

void D3D11Downscaler::setColorTransfer(int transfer)
{
    // Must match the transfer IDs in d3d11_downscale_pixel.hlsl. Unknown SDR
    // follows libplacebo's common gamma-2.2 fallback.
    switch (transfer) {
    case AVCOL_TRC_BT709:
    case AVCOL_TRC_SMPTE170M:
    case AVCOL_TRC_IEC61966_2_4:
    case AVCOL_TRC_BT1361_ECG:
    case AVCOL_TRC_BT2020_10:
    case AVCOL_TRC_BT2020_12:
    case AVCOL_TRC_SMPTE240M: m_Transfer = 1; break;
    case AVCOL_TRC_IEC61966_2_1: m_Transfer = 2; break;
    case AVCOL_TRC_GAMMA28: m_Transfer = 3; break;
    case AVCOL_TRC_LINEAR: m_Transfer = 4; break;
    case AVCOL_TRC_SMPTE2084: m_Transfer = 5; break;
    case AVCOL_TRC_ARIB_STD_B67: m_Transfer = 6; break;
    default: m_Transfer = 0; break;
    }
}

bool D3D11Downscaler::createWeights(ID3D11Device* device, int source, int destination,
                                   Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& view, int& taps)
{
    const double ratio = std::max(1.0, double(source) / destination);
    const double radius = m_Filter == StreamingPreferences::DF_LANCZOS ? 3.0 : 2.0;
    const double support = radius * ratio;
    // Clamped edge samples are combined, so even extreme reductions need no
    // more entries than there are actual source pixels.
    taps = source == destination ? 1 : std::min(source, int(std::ceil(2 * support)) + 1);
    if (taps > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            destination > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        return false;
    }
    struct Weight { float value = 0; float index = 0; };
    std::vector<Weight> weights(size_t(taps) * destination);
    for (int output = 0; output < destination; ++output) {
        if (source == destination) {
            weights[output] = {1.0f, float(output)};
            continue;
        }
        const double center = (output + 0.5) * source / destination - 0.5;
        const int first = int(std::ceil(center - support));
        const int last = int(std::floor(center + support));
        const int clampedFirst = std::max(0, first);
        double sum = 0.0;
        Weight* row = weights.data() + size_t(output) * taps;
        for (int tap = 0; tap < taps; ++tap) row[tap].index = float(std::min(clampedFirst + tap, source - 1));
        for (int input = first; input <= last; ++input) {
            const double weight = kernel(m_Filter, (input - center) / ratio);
            const int index = std::clamp(input, 0, source - 1) - clampedFirst;
            row[index].value += float(weight);
            sum += weight;
        }
        for (int tap = 0; tap < taps; ++tap) row[tap].value /= float(sum);
    }
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = taps;
    desc.Height = destination;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data = {};
    data.pSysMem = weights.data();
    data.SysMemPitch = taps * sizeof(Weight);
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    view.Reset();
    return SUCCEEDED(device->CreateTexture2D(&desc, &data, &texture)) &&
           SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &view));
}

bool D3D11Downscaler::configureOutput(ID3D11Device* device, ID3D11DeviceContext*)
{
    // Float32 retains dark PQ values and signed filter lobes between passes.
    return createTarget(device, m_DstWidth, m_Source.height, DXGI_FORMAT_R32G32B32A32_FLOAT,
                        D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, m_Horizontal) &&
           createWeights(device, m_Source.width, m_DstWidth, m_HorizontalWeights, m_HorizontalTaps) &&
           createWeights(device, m_Source.height, m_DstHeight, m_VerticalWeights, m_VerticalTaps);
}

void D3D11Downscaler::scale(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
                           bool, bool dither, float ditherLevels, const D3D11_VIEWPORT& fullViewport)
{
    ConstBuf constants = {};
    constants.transfer = m_Transfer;
    constants.ditherLevels = ditherLevels;
    constants.taps = m_HorizontalTaps;
    context->UpdateSubresource(m_ConstantBuffer.Get(), 0, nullptr, &constants, 0, 0);
    context->PSSetConstantBuffers(2, 1, m_ConstantBuffer.GetAddressOf());
    const D3D11_VIEWPORT horizontalViewport = {0, 0, float(m_DstWidth), float(m_Source.height), 0, 1};
    context->OMSetRenderTargets(1, m_Horizontal.rtv.GetAddressOf(), nullptr);
    context->RSSetViewports(1, &horizontalViewport);
    context->PSSetShader(m_FilterShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* horizontalInputs[] = {m_Source.srv.Get(), m_HorizontalWeights.Get()};
    context->PSSetShaderResources(0, 2, horizontalInputs);
    drawQuad(context);

    ID3D11ShaderResourceView* nullInputs[2] = {};
    context->PSSetShaderResources(0, 2, nullInputs);
    constants.destinationOffset[0] = m_DstX;
    constants.destinationOffset[1] = m_DstY;
    constants.vertical = 1;
    constants.taps = m_VerticalTaps;
    context->UpdateSubresource(m_ConstantBuffer.Get(), 0, nullptr, &constants, 0, 0);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->RSSetViewports(1, &m_DestinationViewport);
    context->PSSetShader(dither ? m_FilterDitherShader.Get() : m_FilterShader.Get(), nullptr, 0);
    ID3D11ShaderResourceView* verticalInputs[] = {m_Horizontal.srv.Get(), m_VerticalWeights.Get()};
    context->PSSetShaderResources(0, 2, verticalInputs);
    drawQuad(context);
    context->PSSetShaderResources(0, 2, nullInputs);
    context->RSSetViewports(1, &fullViewport);
}
