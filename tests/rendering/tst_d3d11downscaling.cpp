// Headless GPU regression: run the production scaler on WARP, compare to an
// independent CPU reference and libplacebo, and check resources/letterboxing.
#include "streaming/video/ffmpeg-renderers/d3d11downscaler.h"
#include "path.h"

#include <QCoreApplication>
#include <QDir>
#include <d3dcompiler.h>
#include <d3d11sdklayers.h>
#include <libplacebo/renderer.h>
#include <libplacebo/vulkan.h>
extern "C" {
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
using Pixel = std::array<float, 4>;

void require(bool result, const char* message)
{
    if (!result) throw std::runtime_error(message);
}

double weight(int filter, double x)
{
    x = std::abs(x);
    if (filter == 3) {
        if (x >= 3) return 0;
        if (x < 1e-12) return 1;
        constexpr double pi = 3.14159265358979323846;
        return (std::sin(pi*x)/(pi*x)) * (std::sin(pi*x/3)/(pi*x/3));
    }
    // Independent polynomial coefficients for B-spline / Mitchell.
    if (filter == 1) {
        if (x < 1) return (4 - 6*x*x + 3*x*x*x)/6;
        if (x < 2) return std::pow(2-x, 3)/6;
    }
    else {
        if (x < 1) return 8.0/9 - 2*x*x + 7.0/6*x*x*x;
        if (x < 2) return 16.0/9 - 10.0/3*x + 2*x*x - 7.0/18*x*x*x;
    }
    return 0;
}

double decode(double x, int transfer)
{
    if (transfer == AVCOL_TRC_LINEAR) return x;
    if (transfer == AVCOL_TRC_IEC61966_2_1) return x <= .04045 ? x/12.92 : std::pow((x+.055)/1.055, 2.4);
    if (transfer == AVCOL_TRC_SMPTE2084) {
        const double p = std::pow(x, 1/78.84375);
        return std::pow(std::max(p-.8359375, 0.0)/(18.8515625-18.6875*p), 1/.1593017578125);
    }
    return std::pow(x, transfer == AVCOL_TRC_BT709 ? 2.4 : 2.2);
}

double encode(double x, int transfer)
{
    x = std::clamp(x, 0.0, 1.0);
    if (transfer == AVCOL_TRC_LINEAR) return x;
    if (transfer == AVCOL_TRC_IEC61966_2_1) return x <= .0031308 ? 12.92*x : 1.055*std::pow(x, 1/2.4)-.055;
    if (transfer == AVCOL_TRC_SMPTE2084) {
        const double p = std::pow(x, .1593017578125);
        return std::pow((.8359375+18.8515625*p)/(1+18.6875*p), 78.84375);
    }
    return std::pow(x, 1/(transfer == AVCOL_TRC_BT709 ? 2.4 : 2.2));
}

Pixel reference(const std::vector<Pixel>& image, int sw, int sh, int dw, int dh,
                int px, int py, int filter, int transfer)
{
    const double rx = double(sw)/dw, ry = double(sh)/dh;
    const double cx = (px+.5)*rx-.5, cy = (py+.5)*ry-.5;
    const double radius = filter == 3 ? 3 : 2;
    std::array<double, 3> result{};
    double sum = 0;
    for (int y = sh == dh ? py : int(std::ceil(cy-radius*ry)); y <= (sh == dh ? py : int(std::floor(cy+radius*ry))); ++y) {
        for (int x = sw == dw ? px : int(std::ceil(cx-radius*rx)); x <= (sw == dw ? px : int(std::floor(cx+radius*rx))); ++x) {
            const double w = (sw == dw ? 1 : weight(filter, (x-cx)/rx)) * (sh == dh ? 1 : weight(filter, (y-cy)/ry));
            const auto& pixel = image[size_t(std::clamp(y, 0, sh-1))*sw+std::clamp(x, 0, sw-1)];
            for (int c = 0; c < 3; ++c) result[c] += w*decode(pixel[c], transfer);
            sum += w;
        }
    }
    return {float(encode(result[0]/sum, transfer)), float(encode(result[1]/sum, transfer)),
            float(encode(result[2]/sum, transfer)), 1};
}

struct Gpu {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11InfoQueue> debug;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> copy;
    ComPtr<ID3D11InputLayout> layout;
    ComPtr<ID3D11Buffer> indices;
    ComPtr<ID3D11RasterizerState> rasterizer;
    pl_vulkan vk = nullptr;
    pl_renderer renderer = nullptr;
    pl_log log = nullptr;
    HMODULE loader = nullptr;

    Gpu() {
        D3D_FEATURE_LEVEL level;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                      D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION,
                                      &device, &level, &context);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, &level, &context);
        }
        require(SUCCEEDED(hr), "WARP device creation failed");
        device.As(&debug);
        const QByteArray code = Path::readDataFile("d3d11_vertex.fxc");
        require(SUCCEEDED(device->CreateVertexShader(code.data(), code.size(), nullptr, &vertex)), "vertex shader failed");
        D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0}
        };
        require(SUCCEEDED(device->CreateInputLayout(elements, 2, code.data(), code.size(), &layout)), "input layout failed");
        const char* copyCode = "Texture2D<float4> tex:register(t0); float4 main(float4 p:SV_POSITION):SV_TARGET { return tex.Load(int3(int2(p.xy),0)); }";
        ComPtr<ID3DBlob> blob, errors;
        require(SUCCEEDED(D3DCompile(copyCode, std::strlen(copyCode), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &errors)), "copy shader compile failed");
        require(SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &copy)), "copy shader failed");
        const uint16_t values[] = {0,1,2,2,1,3};
        D3D11_BUFFER_DESC ib{};
        ib.ByteWidth = sizeof(values); ib.Usage = D3D11_USAGE_IMMUTABLE; ib.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA data{}; data.pSysMem = values;
        require(SUCCEEDED(device->CreateBuffer(&ib, &data, &indices)), "index buffer failed");
        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID; rs.CullMode = D3D11_CULL_NONE; rs.DepthClipEnable = TRUE;
        require(SUCCEEDED(device->CreateRasterizerState(&rs, &rasterizer)), "rasterizer failed");
        loader = LoadLibraryW(L"vulkan-1.dll");
        require(loader != nullptr, "Vulkan loader unavailable");
        pl_log_params logging{}; logging.log_cb = pl_log_simple; logging.log_level = PL_LOG_WARN;
        log = pl_log_create(PL_API_VER, &logging);
        pl_vulkan_params vulkan = pl_vulkan_default_params;
        vulkan.get_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader,"vkGetInstanceProcAddr"));
        vk = pl_vulkan_create(log, &vulkan);
        require(vk != nullptr, "Vulkan/libplacebo device creation failed");
        renderer = pl_renderer_create(log, vk->gpu);
        require(renderer != nullptr, "libplacebo renderer creation failed");
    }
    ~Gpu() {
        if (renderer) pl_renderer_destroy(&renderer);
        if (vk) pl_vulkan_destroy(&vk);
        if (log) pl_log_destroy(&log);
        if (loader) FreeLibrary(loader);
    }

    std::vector<Pixel> placebo(const std::vector<Pixel>& image, int sw, int sh, int dw, int dh, int filter) {
        pl_tex_params params{};
        params.w = sw; params.h = sh;
        params.format = pl_find_named_fmt(vk->gpu, "rgba32f");
        params.sampleable = true; params.initial_data = image.data();
        pl_tex source = pl_tex_create(vk->gpu, &params);
        params.w = dw; params.h = dh; params.initial_data = nullptr;
        params.renderable = true; params.host_readable = true;
        pl_tex target = pl_tex_create(vk->gpu, &params);
        require(source && target, "libplacebo texture creation failed");
        pl_frame src{}, dst{};
        src.num_planes = dst.num_planes = 1;
        src.planes[0].texture = source; dst.planes[0].texture = target;
        src.planes[0].components = dst.planes[0].components = 3;
        for (int c = 0; c < 3; ++c) src.planes[0].component_mapping[c] = dst.planes[0].component_mapping[c] = c;
        src.repr.sys = dst.repr.sys = PL_COLOR_SYSTEM_RGB;
        src.repr.levels = dst.repr.levels = PL_COLOR_LEVELS_FULL;
        src.color.primaries = dst.color.primaries = PL_COLOR_PRIM_BT_709;
        src.color.transfer = dst.color.transfer = PL_COLOR_TRC_GAMMA22;
        pl_render_params render = pl_render_fast_params;
        render.force_low_bit_depth_fbos = false;
        render.downscaler = filter == 1 ? &pl_filter_bicubic : filter == 2 ? &pl_filter_mitchell : &pl_filter_lanczos;
        require(pl_render_image(renderer, &src, &dst, &render), "libplacebo render failed");
        std::vector<Pixel> result(size_t(dw)*dh);
        pl_tex_transfer_params read{}; read.tex = target; read.ptr = result.data();
        require(pl_tex_download(vk->gpu, &read), "libplacebo readback failed");
        pl_tex_destroy(vk->gpu, &source); pl_tex_destroy(vk->gpu, &target);
        return result;
    }

    std::vector<Pixel> draw(D3D11Downscaler& scaler, const std::vector<Pixel>& image,
                            int sw, int sh, int dw, int dh, int transfer, bool dither) {
        context->ClearState();
        if (debug) debug->ClearStoredMessages();
        require(scaler.configure(device.Get(), context.Get(), sw, sh, 3, 2, dw, dh, 1, 1) && scaler.active(), "scaler configure failed");
        scaler.setColorTransfer(transfer);
        context->IASetInputLayout(layout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetIndexBuffer(indices.Get(), DXGI_FORMAT_R16_UINT, 0);
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->RSSetState(rasterizer.Get());
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = sw; desc.Height = sh; desc.ArraySize = desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA upload{}; upload.pSysMem = image.data(); upload.SysMemPitch = sw*sizeof(Pixel);
        ComPtr<ID3D11Texture2D> source, output, staging;
        ComPtr<ID3D11ShaderResourceView> sourceView;
        ComPtr<ID3D11RenderTargetView> outputView;
        require(SUCCEEDED(device->CreateTexture2D(&desc, &upload, &source)) &&
                SUCCEEDED(device->CreateShaderResourceView(source.Get(), nullptr, &sourceView)), "upload failed");
        scaler.beginSourcePass(context.Get());
        context->PSSetShader(copy.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, sourceView.GetAddressOf());
        context->DrawIndexed(6, 0, 0);
        ID3D11ShaderResourceView* nullView = nullptr;
        context->PSSetShaderResources(0, 1, &nullView);
        desc.Width = dw+6; desc.Height = dh+4; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &output)) &&
                SUCCEEDED(device->CreateRenderTargetView(output.Get(), nullptr, &outputView)), "output target failed");
        const float border[] = {.125f,.25f,.375f,1};
        context->ClearRenderTargetView(outputView.Get(), border);
        // Dither inputs: fixed threshold, no grain. No per-frame noise is
        // allowed to leak into the source or horizontal filtering passes.
        ComPtr<ID3D11Texture2D> thresholds;
        ComPtr<ID3D11ShaderResourceView> thresholdView;
        ComPtr<ID3D11Buffer> ditherConstants;
        if (dither) {
            D3D11_TEXTURE2D_DESC threshold{};
            threshold.Width = threshold.Height = threshold.ArraySize = threshold.MipLevels = 1;
            threshold.Format = DXGI_FORMAT_R32_FLOAT; threshold.SampleDesc.Count = 1;
            threshold.Usage = D3D11_USAGE_IMMUTABLE; threshold.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const float half = .5f; D3D11_SUBRESOURCE_DATA td{}; td.pSysMem = &half; td.SysMemPitch = sizeof(float);
            require(SUCCEEDED(device->CreateTexture2D(&threshold, &td, &thresholds)) &&
                    SUCCEEDED(device->CreateShaderResourceView(thresholds.Get(), nullptr, &thresholdView)), "threshold texture failed");
            ID3D11ShaderResourceView* views[] = {thresholdView.Get(),thresholdView.Get()};
            context->PSSetShaderResources(3, 2, views);
            const float zeros[8] = {};
            D3D11_BUFFER_DESC cb{}; cb.ByteWidth = sizeof(zeros); cb.Usage = D3D11_USAGE_IMMUTABLE; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA cd{}; cd.pSysMem = zeros;
            require(SUCCEEDED(device->CreateBuffer(&cb, &cd, &ditherConstants)), "dither constants failed");
            context->PSSetConstantBuffers(1, 1, ditherConstants.GetAddressOf());
        }
        D3D11_VIEWPORT viewport{0,0,float(dw+6),float(dh+4),0,1};
        scaler.scale(context.Get(), outputView.Get(), transfer == AVCOL_TRC_SMPTE2084,
                     dither, transfer == AVCOL_TRC_SMPTE2084 ? 1023.f : 255.f, viewport);
        D3D11_VIEWPORT restored{}; UINT count = 1; context->RSGetViewports(&count, &restored);
        require(restored.Width == viewport.Width && restored.Height == viewport.Height && restored.TopLeftX == 0 && restored.TopLeftY == 0, "overlay viewport not restored");
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)), "staging failed");
        context->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapping{};
        require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping)), "readback failed");
        std::vector<Pixel> result(size_t(dw)*dh);
        for (int y = 0; y < dh+4; ++y) {
            auto row = reinterpret_cast<const Pixel*>(static_cast<const char*>(mapping.pData)+size_t(y)*mapping.RowPitch);
            for (int x = 0; x < dw+6; ++x) {
                if (x >= 3 && x < dw+3 && y >= 2 && y < dh+2) result[size_t(y-2)*dw+x-3] = row[x];
                else require(row[x] == Pixel{border[0],border[1],border[2],border[3]}, "letterbox modified");
            }
        }
        context->Unmap(staging.Get(), 0);
        if (debug) {
            for (UINT64 i = 0; i < debug->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
                SIZE_T bytes = 0; debug->GetMessage(i, nullptr, &bytes);
                std::vector<char> storage(bytes); auto message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
                debug->GetMessage(i, message, &bytes);
                if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                    std::fprintf(stderr, "D3D11: %s\n", message->pDescription);
                    require(false, "GPU validation message");
                }
            }
        }
        return result;
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    require(argc == 2, "Pass the app/shaders directory as the sole argument");
    QDir::setCurrent(QString::fromLocal8Bit(argv[1]));
    try {
        Gpu gpu;
        int cases = 0;
        for (int filter = 1; filter <= 3; ++filter) {
            D3D11Downscaler scaler;
            require(scaler.initialize(gpu.device.Get(), filter), "scaler shader load failed");
            for (const auto& size : {std::array<int,4>{32,20,16,10}, {53,32,26,15}, {17,9,1,1}, {32,20,32,9}}) {
                const int sw=size[0], sh=size[1], dw=size[2], dh=size[3];
                std::vector<Pixel> input(size_t(sw)*sh);
                for (int y=0; y<sh; ++y) for (int x=0; x<sw; ++x) {
                    input[size_t(y)*sw+x] = {float((x*3+y)%5)/4, float((x+y*2)%5)/4, float((x*2+y*3)%5)/4, 1};
                }
                for (int transfer : {AVCOL_TRC_LINEAR, AVCOL_TRC_GAMMA22, AVCOL_TRC_BT709, AVCOL_TRC_IEC61966_2_1, AVCOL_TRC_SMPTE2084}) {
                    for (bool dither : {false,true}) {
                        const auto actual = gpu.draw(scaler, input, sw, sh, dw, dh, transfer, dither);
                        double error = 0;
                        for (int y=0; y<dh; ++y) for (int x=0; x<dw; ++x) {
                            auto expected = reference(input, sw, sh, dw, dh, x, y, filter, transfer);
                            for (int c=0; c<3; ++c) {
                                const float levels = transfer == AVCOL_TRC_SMPTE2084 ? 1023.f : 255.f;
                                if (dither) expected[c] = std::floor(expected[c]*levels+.5f)/levels;
                                require(std::isfinite(actual[size_t(y)*dw+x][c]), "non-finite output");
                                error = std::max(error, double(std::abs(actual[size_t(y)*dw+x][c]-expected[c])));
                            }
                        }
                        if (error > (dither ? 1.0/255+1e-5 : .0006)) {
                            std::fprintf(stderr, "filter=%d size=%dx%d->%dx%d transfer=%d dither=%d error=%.8f\n", filter,sw,sh,dw,dh,transfer,dither,error);
                            require(false, "GPU differs from CPU reference");
                        }
                        ++cases;
                    }
                    if (transfer == AVCOL_TRC_GAMMA22 && dw > 1) {
                        auto actual = gpu.draw(scaler, input, sw, sh, dw, dh, transfer, false);
                        auto expected = gpu.placebo(input, sw, sh, dw, dh, filter);
                        double error = 0;
                        for (size_t p=0; p<actual.size(); ++p) for (int c=0; c<3; ++c) error = std::max(error, double(std::abs(actual[p][c]-expected[p][c])));
                        std::printf("libplacebo comparison: filter=%d %dx%d->%dx%d max error %.6f\n", filter,sw,sh,dw,dh,error);
                        require(error < .003, "D3D11 differs from libplacebo");
                        ++cases;
                    }
                }
            }
            // Full Deck-sized noninteger reduction and dark-PQ preservation.
            std::vector<Pixel> constant(size_t(2650)*1600, Pixel{.015625f,.03125f,.0625f,1});
            auto output = gpu.draw(scaler, constant, 2650,1600,1280,773,AVCOL_TRC_SMPTE2084,false);
            for (const auto& p : output) for (int c=0; c<3; ++c) require(std::abs(p[c]-constant[0][c]) < .0001, "dark PQ constant changed");
            require(scaler.configure(gpu.device.Get(),gpu.context.Get(),32,20,0,0,32,20,1,1) && !scaler.active(), "native-size filtering active");
            require(scaler.configure(gpu.device.Get(),gpu.context.Get(),32,20,0,0,64,40,1,1) && !scaler.active(), "upscaling incorrectly filtered");
            require(scaler.configure(gpu.device.Get(),gpu.context.Get(),0,0,0,0,0,0,1,1) && !scaler.active(), "zero-sized configure active");
            ++cases;
        }
        std::printf("PASS: %d GPU downscaling cases (CPU reference, libplacebo, SDR/PQ, dithering, letterbox, resizing); D3D11 debug layer %s\n", cases, gpu.debug ? "enabled" : "unavailable");
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr,"FAIL: %s\n",e.what()); return 1; }
}
