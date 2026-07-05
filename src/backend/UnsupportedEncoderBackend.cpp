#include "backend/UnsupportedEncoderBackend.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

namespace D3DVideoEncoderLib {

UnsupportedEncoderBackend::UnsupportedEncoderBackend(D3DVideoEncoderBackendType backendType, DebugLog log)
    : backendType_(backendType), log_(log) {}

void UnsupportedEncoderBackend::initialize(const D3D11VideoEncoderDesc& desc) {
    desc_ = desc;
    throwUnsupported();
}

EncodeSurface::Api UnsupportedEncoderBackend::requiredSurfaceApi() const {
    switch (backendType_) {
    case D3DVideoEncoderBackendType::NvencD3D12:
    case D3DVideoEncoderBackendType::D3D12VideoEncode:
        return EncodeSurface::Api::D3D12;
    default:
        return EncodeSurface::Api::D3D11;
    }
}

void UnsupportedEncoderBackend::encode(const EncodeSurface&, int64_t, int64_t) {
    throwUnsupported();
}

void UnsupportedEncoderBackend::flush() {}
void UnsupportedEncoderBackend::close() {}

void UnsupportedEncoderBackend::throwUnsupported() const {
    (void)log_;
    switch (backendType_) {
    case D3DVideoEncoderBackendType::NvencD3D11:
    case D3DVideoEncoderBackendType::NvencD3D12:
        throw D3DVideoEncoderError(
            "NVENC backend is scaffolded but not compiled into this package. "
            "Add NVIDIA Video Codec SDK nvEncodeAPI.h/lib and implement/enable D3DVIDEOENCODER_ENABLE_NVENC.");
    case D3DVideoEncoderBackendType::D3D12VideoEncode:
        throw D3DVideoEncoderError(
            "D3D12 Video Encode backend is scaffolded but not implemented in this package. "
            "Use MediaFoundation or NVENC first; D3D12 Video Encode requires a dedicated command/bitstream implementation.");
    default:
        throw D3DVideoEncoderError("Unsupported encoder backend.");
    }
}

} // namespace D3DVideoEncoderLib
