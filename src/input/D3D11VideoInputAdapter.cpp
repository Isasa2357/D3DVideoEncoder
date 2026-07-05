#include "input/D3D11VideoInputAdapter.hpp"

#include <sstream>

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

namespace D3DVideoEncoderLib {

void D3D11VideoInputAdapter::initialize(const D3D11VideoEncoderDesc& desc, DebugLog log) {
    desc_ = desc;
    log_ = log;
    core_ = desc_.input.core;

    if (!core_) {
        throw D3DVideoEncoderError("D3D11VideoInputAdapter requires desc.d3d11.core. This is required so D3D11Helper owns resource creation and processing.");
    }
    if (!IsYuv420EncodeFormat(desc_.internalFormat)) {
        throw D3DVideoEncoderError("D3D11VideoInputAdapter supports only internalFormat NV12/P010.");
    }
    if (!IsSupportedD3D11InputFormat(desc_.input.inputFormat)) {
        throw D3DVideoEncoderError("Unsupported D3D11 input format. Use NV12/P010/BGRA8/RGBA8/RGBA16F.");
    }

    surfacePool_.initialize(*core_, desc_.width, desc_.height, ToDxgiFormat(desc_.internalFormat), desc_.queueDepth, true);

    if (desc_.input.inputFormat != ToDxgiFormat(desc_.internalFormat)) {
        initializeConverterIfNeeded(desc_.input.inputFormat);
    }

    log_.info("D3D11VideoInputAdapter initialized");
}

void D3D11VideoInputAdapter::initializeConverterIfNeeded(DXGI_FORMAT inputFormat) {
    if (converterInitialized_) return;

    if (!desc_.input.allowFormatConversion) {
        throw D3DVideoEncoderError("Input texture is not NV12 and desc.d3d11.allowFormatConversion is false.");
    }
    if (inputFormat != DXGI_FORMAT_B8G8R8A8_UNORM && inputFormat != DXGI_FORMAT_R8G8B8A8_UNORM && inputFormat != DXGI_FORMAT_R16G16B16A16_FLOAT) {
        throw D3DVideoEncoderError("D3D11 format conversion supports BGRA8/RGBA8/RGBA16F to NV12/P010.");
    }

    processingContext_.Initialize(*core_, desc_.input.processingShaderDirectory);
    if (desc_.internalFormat == VideoPixelFormat::NV12 && !processingContext_.SupportsNv12Uav()) {
        throw D3DVideoEncoderError("D3D11 device does not support NV12 UAV required for conversion.");
    }
    if (desc_.internalFormat == VideoPixelFormat::P010 && !processingContext_.SupportsP010Uav()) {
        throw D3DVideoEncoderError("D3D11 device does not support P010 UAV required for conversion.");
    }

    formatConverter_.Initialize(processingContext_);
    converterInitialized_ = true;
    log_.info("D3D11Processing format converter initialized");
}

void D3D11VideoInputAdapter::validateTexture(ID3D11Texture2D* texture, D3D11_TEXTURE2D_DESC& outDesc) const {
    if (!texture) {
        throw D3DVideoEncoderError("write() received null ID3D11Texture2D.");
    }

    texture->GetDesc(&outDesc);

    if (outDesc.Width != desc_.width || outDesc.Height != desc_.height) {
        std::ostringstream oss;
        oss << "Input texture size mismatch. expected=" << desc_.width << "x" << desc_.height
            << " actual=" << outDesc.Width << "x" << outDesc.Height;
        throw D3DVideoEncoderError(oss.str());
    }

    if (outDesc.Format != desc_.input.inputFormat) {
        std::ostringstream oss;
        oss << "Input texture format mismatch. expected DXGI_FORMAT value="
            << static_cast<int>(desc_.input.inputFormat)
            << " actual=" << static_cast<int>(outDesc.Format);
        throw D3DVideoEncoderError(oss.str());
    }

    Microsoft::WRL::ComPtr<ID3D11Device> texDevice;
    texture->GetDevice(&texDevice);
    if (texDevice.Get() != core_->GetDevice()) {
        throw D3DVideoEncoderError("Input texture belongs to a different ID3D11Device than desc.d3d11.core.");
    }
}

EncodeSurface D3D11VideoInputAdapter::prepare(ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC texDesc = {};
    validateTexture(texture, texDesc);

    EncodeSurface dst = surfacePool_.acquire();
    ID3D11DeviceContext* context = core_->GetImmediateContext();

    if (texDesc.Format == ToDxgiFormat(desc_.internalFormat)) {
        context->CopyResource(dst.d3d11Texture.Get(), texture);
        core_->Flush();
        return dst;
    }

    initializeConverterIfNeeded(texDesc.Format);

    Microsoft::WRL::ComPtr<ID3D11Texture2D> srcTexture(texture);
    D3D11CoreLib::D3D11Resource srcResource(srcTexture);

    D3D11CoreLib::Processing::FormatConvertDesc convertDesc = {};
    convertDesc.srcFormat = texDesc.Format;
    convertDesc.dstFormat = ToDxgiFormat(desc_.internalFormat);
    convertDesc.color.srcMatrix = toProcessingMatrix(desc_.colorMatrix);
    convertDesc.color.dstMatrix = toProcessingMatrix(desc_.colorMatrix);
    convertDesc.color.srcRange = D3D11CoreLib::Processing::ProcessingColorRange::Full;
    convertDesc.color.dstRange = toProcessingRange(desc_.colorRange);
    convertDesc.color.alphaMode = D3D11CoreLib::Processing::ProcessingAlphaMode::Ignore;
    convertDesc.srcRect = { 0, 0, desc_.width, desc_.height };
    convertDesc.dstRect = { 0, 0, desc_.width, desc_.height };

    formatConverter_.DispatchConvert(
        context,
        srcResource,
        dst.d3d11Resource,
        convertDesc);

    // Input preparation is synchronous. Flush keeps Media Foundation from seeing a surface
    // before copy/convert commands have reached the GPU.
    core_->Flush();
    return dst;
}

void D3D11VideoInputAdapter::release(const EncodeSurface& surface) {
    surfacePool_.release(surface);
}

void D3D11VideoInputAdapter::flush() {
    if (core_) {
        core_->Flush();
    }
}

D3D11CoreLib::Processing::ProcessingColorMatrix D3D11VideoInputAdapter::toProcessingMatrix(VideoColorMatrix matrix) const noexcept {
    switch (matrix) {
    case VideoColorMatrix::BT601: return D3D11CoreLib::Processing::ProcessingColorMatrix::BT601;
    case VideoColorMatrix::BT709: return D3D11CoreLib::Processing::ProcessingColorMatrix::BT709;
    case VideoColorMatrix::BT2020: return D3D11CoreLib::Processing::ProcessingColorMatrix::BT2020;
    default: return D3D11CoreLib::Processing::ProcessingColorMatrix::BT709;
    }
}

D3D11CoreLib::Processing::ProcessingColorRange D3D11VideoInputAdapter::toProcessingRange(VideoColorRange range) const noexcept {
    switch (range) {
    case VideoColorRange::Full: return D3D11CoreLib::Processing::ProcessingColorRange::Full;
    case VideoColorRange::Limited: return D3D11CoreLib::Processing::ProcessingColorRange::Limited;
    default: return D3D11CoreLib::Processing::ProcessingColorRange::Limited;
    }
}

} // namespace D3DVideoEncoderLib
