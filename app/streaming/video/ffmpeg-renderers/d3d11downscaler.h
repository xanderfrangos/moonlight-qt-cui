#pragma once

#include "d3d11upscaler.h"

// Ratio-aware, separable RGB downscaling. Weights are computed once per resize;
// the GPU filters horizontally in linear light, then vertically into the output.
class D3D11Downscaler : public D3D11Upscaler
{
public:
    bool initialize(ID3D11Device* device, int filter);
    const char* name() const override;
    bool handlesPq() const override { return true; }
    void setColorTransfer(int transfer);
    void scale(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
               bool pq, bool dither, float ditherLevels,
               const D3D11_VIEWPORT& fullViewport) override;

protected:
    bool needsScaling(int srcWidth, int srcHeight,
                      int dstWidth, int dstHeight) const override {
        return dstWidth <= srcWidth && dstHeight <= srcHeight &&
               (dstWidth < srcWidth || dstHeight < srcHeight);
    }
    bool configureOutput(ID3D11Device* device, ID3D11DeviceContext* context) override;

private:
    struct ConstBuf {
        int32_t destinationOffset[2];
        int32_t vertical;
        int32_t transfer;
        float ditherLevels;
        int32_t taps;
        int32_t padding[2];
    };
    static_assert(sizeof(ConstBuf) == 32, "Downscaling shader constant layout changed");

    bool createWeights(ID3D11Device* device, int source, int destination,
                       Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& view, int& taps);

    int m_Filter = 0;
    int m_Transfer = 0;
    int m_HorizontalTaps = 0;
    int m_VerticalTaps = 0;
    Target m_Horizontal;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_HorizontalWeights;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_VerticalWeights;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_FilterShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_FilterDitherShader;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_ConstantBuffer;
};
