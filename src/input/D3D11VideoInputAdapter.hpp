#pragma once

#include "surface/D3D11EncodeSurfacePool.hpp"
#include "util/DebugLog.hpp"

#include <D3DVideoEncoder/D3D11VideoEncoderDesc.hpp>

#include <D3D11Helper/D3D11Processing/D3D11Processing.hpp>

#include <memory>

namespace D3DVideoEncoderLib {

class D3D11VideoInputAdapter {
public:
    D3D11VideoInputAdapter() = default;

    void initialize(const D3D11VideoEncoderDesc& desc, DebugLog log);

    EncodeSurface prepare(ID3D11Texture2D* texture);
    void release(const EncodeSurface& surface);
    void flush();

private:
    void validateTexture(ID3D11Texture2D* texture, D3D11_TEXTURE2D_DESC& outDesc) const;
    void initializeConverterIfNeeded(DXGI_FORMAT inputFormat);
    D3D11CoreLib::Processing::ProcessingColorMatrix toProcessingMatrix(VideoColorMatrix matrix) const noexcept;
    D3D11CoreLib::Processing::ProcessingColorRange toProcessingRange(VideoColorRange range) const noexcept;

    D3D11VideoEncoderDesc desc_ = {};
    D3D11CoreLib::D3D11Core* core_ = nullptr;
    DebugLog log_;

    D3D11EncodeSurfacePool surfacePool_;

    bool converterInitialized_ = false;
    D3D11CoreLib::Processing::D3D11ProcessingContext processingContext_;
    D3D11CoreLib::Processing::D3D11FormatConverter formatConverter_;
};

} // namespace D3DVideoEncoderLib
