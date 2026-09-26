#include "d3d11ls1.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSettings>

#include <SDL.h>

#include <cstring>
#include <initializer_list>

// The resource IDs, register bindings, parameter layout and dispatch sizes
// match the Linux Vulkan implementation in ls1shaders.cpp and ls1vulkan.cpp,
// which follows MAKO's GPL-3.0-or-later LS1 implementation
// (eugeniosegala/MAKO, commit 0534110a381672dc33a285d45a61662ff06f44ae).
namespace {

// The shared reconstruction stage, and the first of each variant's three
// analysis stages
constexpr int k_ReconstructResource = 146;
constexpr int k_FirstVariantResource = 147;

// The feature texture is twice the stream size and can't exceed D3D11's
// 16384 texel limit
constexpr int k_MaxSourceSize = 8192;

constexpr int k_ThreadGroupSize = 16;

QStringList steamLibraries()
{
    QStringList roots;
    const QStringList keys = {
        "HKEY_CURRENT_USER\\Software\\Valve\\Steam|SteamPath",
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Valve\\Steam|InstallPath",
        "HKEY_LOCAL_MACHINE\\SOFTWARE\\Valve\\Steam|InstallPath",
    };
    for (const QString& key : keys) {
        const QStringList parts = key.split('|');
        const QString path = QSettings(parts[0], QSettings::NativeFormat).value(parts[1]).toString();
        if (!path.isEmpty()) {
            roots << QDir::cleanPath(path);
        }
    }

    // Other library folders are listed in the main install's libraryfolders.vdf
    QStringList libraries = roots;
    for (const QString& root : roots) {
        QFile vdf(QDir(root).filePath("steamapps/libraryfolders.vdf"));
        if (!vdf.open(QIODevice::ReadOnly | QIODevice::Text)) {
            continue;
        }
        static const QRegularExpression pathLine("\"path\"\\s+\"([^\"]+)\"");
        auto matches = pathLine.globalMatch(QString::fromUtf8(vdf.readAll()));
        while (matches.hasNext()) {
            QString path = matches.next().captured(1);
            path.replace("\\\\", "\\");
            libraries << QDir::cleanPath(path);
        }
    }

    libraries.removeDuplicates();
    return libraries;
}

}

QString D3D11Ls1Upscaler::findLosslessScalingDll(const QString& configuredPath)
{
    if (!configuredPath.isEmpty()) {
        return configuredPath;
    }

    const QString environment = qEnvironmentVariable("MOONLIGHT_LOSSLESS_SCALING_DLL");
    if (!environment.isEmpty()) {
        return environment;
    }

    for (const QString& library : steamLibraries()) {
        const QString path = QDir(library).filePath("steamapps/common/Lossless Scaling/Lossless.dll");
        if (QFile::exists(path)) {
            return QDir::toNativeSeparators(path);
        }
    }

    return {};
}

bool D3D11Ls1Upscaler::initialize(ID3D11Device* device, const QString& dllPath, int variant,
                                  bool tenBit, QString* error)
{
    auto fail = [error](const QString& message) {
        if (error) {
            *error = message;
        }
        return false;
    };

    if (variant < 0 || variant > 4) {
        return fail(QString("invalid LS1 model variant %1").arg(variant));
    }

    // The stages write these formats through typed UAVs
    for (DXGI_FORMAT format : {DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        UINT support = 0;
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support2 = { format };
        if (FAILED(device->CheckFormatSupport(format, &support)) ||
                !(support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) ||
                FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support2, sizeof(support2))) ||
                !(support2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE)) {
            return fail(QString("the GPU can't write DXGI format %1 from a compute shader").arg(format));
        }
    }

    // Only the DLL's resources are needed, so map it as data without running it
    HMODULE dll = LoadLibraryExW(reinterpret_cast<LPCWSTR>(dllPath.utf16()), nullptr,
                                 LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (dll == nullptr) {
        return fail(QString("can't open %1 (error %2)").arg(dllPath).arg(GetLastError()));
    }

    const int first = k_FirstVariantResource + variant * 3;
    const std::array<int, 4> resources = { first, first + 1, first + 2, k_ReconstructResource };
    QString loadError;
    for (size_t i = 0; i < resources.size(); i++) {
        HRSRC resource = FindResourceW(dll, MAKEINTRESOURCEW(resources[i]), MAKEINTRESOURCEW(10)); // RT_RCDATA
        HGLOBAL loaded = resource ? LoadResource(dll, resource) : nullptr;
        const void* data = loaded ? LockResource(loaded) : nullptr;
        const DWORD size = resource ? SizeofResource(dll, resource) : 0;
        if (data == nullptr || size < 32 || memcmp(data, "DXBC", 4) != 0) {
            loadError = QString("Lossless.dll has no LS1 shader resource %1").arg(resources[i]);
            break;
        }

        HRESULT hr = device->CreateComputeShader(data, size, nullptr, &m_Stages[i]);
        if (FAILED(hr)) {
            loadError = QString("CreateComputeShader() failed for LS1 resource %1: %2")
                            .arg(resources[i]).arg((quint32)hr, 0, 16);
            break;
        }
    }
    FreeLibrary(dll);
    if (!loadError.isEmpty()) {
        return fail(loadError);
    }

    if (!loadPixelShader(device, "d3d11_upscale_copy_pixel.fxc", m_CopyShader) ||
            !loadPixelShader(device, "d3d11_upscale_copy_dither_pixel.fxc", m_CopyDitherShader)) {
        return fail("the copy shaders could not be loaded");
    }

    // The reconstruction stage samples the feature texture
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    HRESULT hr = device->CreateSamplerState(&samplerDesc, &m_Sampler);
    if (FAILED(hr)) {
        return fail(QString("CreateSamplerState() failed: %1").arg((quint32)hr, 0, 16));
    }

    D3D11_BUFFER_DESC paramDesc = {};
    paramDesc.ByteWidth = sizeof(Parameters);
    paramDesc.Usage = D3D11_USAGE_DEFAULT;
    paramDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device->CreateBuffer(&paramDesc, nullptr, &m_ParameterBuffer);
    if (FAILED(hr)) {
        return fail(QString("CreateBuffer() failed for the LS1 parameters: %1").arg((quint32)hr, 0, 16));
    }

    m_Copy.ditherLevels = 255.0f;
    D3D11_BUFFER_DESC copyDesc = {};
    copyDesc.ByteWidth = sizeof(CopyConstBuf);
    copyDesc.Usage = D3D11_USAGE_DEFAULT;
    copyDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA copyData = {};
    copyData.pSysMem = &m_Copy;
    hr = device->CreateBuffer(&copyDesc, &copyData, &m_CopyBuffer);
    if (FAILED(hr)) {
        return fail(QString("CreateBuffer() failed for the LS1 copy constants: %1").arg((quint32)hr, 0, 16));
    }

    // Match the stream's precision for the color conversion output
    m_SourceFormat = tenBit ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    return true;
}

bool D3D11Ls1Upscaler::configureOutput(ID3D11Device* device, ID3D11DeviceContext* context)
{
    const int w = m_Source.width;
    const int h = m_Source.height;
    if (w > k_MaxSourceSize || h > k_MaxSourceSize) {
        SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                    "LS1 can't upscale a %dx%d stream", w, h);
        return false;
    }

    const UINT computeBinds = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (m_IntermediateA.width != w || m_IntermediateA.height != h) {
        if (!createTarget(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, computeBinds, m_IntermediateA) ||
                !createTarget(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, computeBinds, m_IntermediateB) ||
                !createTarget(device, w * 2, h * 2, DXGI_FORMAT_R8_SNORM, computeBinds, m_Feature)) {
            m_IntermediateA = {};
            return false;
        }
    }
    if ((m_Output.width != m_DstWidth || m_Output.height != m_DstHeight) &&
            !createTarget(device, m_DstWidth, m_DstHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
                          computeBinds, m_Output)) {
        return false;
    }

    // The whole stream is the source, with no crop or offset
    const Parameters parameters = {
        (uint32_t)w, (uint32_t)h, (uint32_t)w, (uint32_t)h,
        0, 0, 0, 0,
        (uint32_t)m_DstWidth, (uint32_t)m_DstHeight, 0, 0,
    };
    context->UpdateSubresource(m_ParameterBuffer.Get(), 0, nullptr, &parameters, 0, 0);

    m_Copy.dstOffset[0] = m_DstX;
    m_Copy.dstOffset[1] = m_DstY;
    context->UpdateSubresource(m_CopyBuffer.Get(), 0, nullptr, &m_Copy, 0, 0);
    return true;
}

void D3D11Ls1Upscaler::dispatch(ID3D11DeviceContext* context, int stage,
                                ID3D11ShaderResourceView* input0, ID3D11ShaderResourceView* input1,
                                ID3D11UnorderedAccessView* output, int width, int height)
{
    // Bind the output first. That releases the previous stage's output, which
    // is this stage's input and can't be read while it's still bound for writing.
    context->CSSetShader(m_Stages[stage].Get(), nullptr, 0);
    context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    ID3D11ShaderResourceView* inputs[2] = { input0, input1 };
    context->CSSetShaderResources(0, 2, inputs);
    context->Dispatch((width + k_ThreadGroupSize - 1) / k_ThreadGroupSize,
                      (height + k_ThreadGroupSize - 1) / k_ThreadGroupSize, 1);

    ID3D11ShaderResourceView* nullInputs[2] = {};
    context->CSSetShaderResources(0, 2, nullInputs);
}

void D3D11Ls1Upscaler::upscale(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
                               bool, bool dither, float ditherLevels,
                               const D3D11_VIEWPORT& fullViewport)
{
    // Release the color conversion output so the first stage can read it
    context->OMSetRenderTargets(0, nullptr, nullptr);

    context->CSSetConstantBuffers(0, 1, m_ParameterBuffer.GetAddressOf());
    context->CSSetSamplers(0, 1, m_Sampler.GetAddressOf());

    const int w = m_Source.width;
    const int h = m_Source.height;
    dispatch(context, 0, m_Source.srv.Get(), nullptr, m_IntermediateA.uav.Get(), w, h);
    dispatch(context, 1, m_IntermediateA.srv.Get(), nullptr, m_IntermediateB.uav.Get(), w, h);
    dispatch(context, 2, m_IntermediateB.srv.Get(), nullptr, m_Feature.uav.Get(), w, h);
    dispatch(context, 3, m_Feature.srv.Get(), m_Source.srv.Get(), m_Output.uav.Get(), m_DstWidth, m_DstHeight);

    ID3D11UnorderedAccessView* nullUav = nullptr;
    context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);

    // The display's depth can change while streaming
    if (dither && ditherLevels != m_Copy.ditherLevels) {
        m_Copy.ditherLevels = ditherLevels;
        context->UpdateSubresource(m_CopyBuffer.Get(), 0, nullptr, &m_Copy, 0, 0);
    }

    // Copy into the destination rectangle of the back buffer
    context->OMSetRenderTargets(1, &target, nullptr);
    context->RSSetViewports(1, &m_DestinationViewport);
    context->PSSetShader(dither ? m_CopyDitherShader.Get() : m_CopyShader.Get(), nullptr, 0);
    context->PSSetConstantBuffers(2, 1, m_CopyBuffer.GetAddressOf());
    context->PSSetShaderResources(0, 1, m_Output.srv.GetAddressOf());
    drawQuad(context);

    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->PSSetShaderResources(0, 1, &nullSrv);
    context->RSSetViewports(1, &fullViewport);
}
