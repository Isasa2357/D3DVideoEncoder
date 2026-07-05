#include "backend/nvenc/NvencD3D11EncoderBackend.hpp"

#include <sstream>

namespace D3DVideoEncoderLib {

NvencD3D11EncoderBackend::NvencD3D11EncoderBackend(DebugLog log) : log_(log) {}
NvencD3D11EncoderBackend::~NvencD3D11EncoderBackend() {
    try { close(); } catch (...) {}
}

void NvencD3D11EncoderBackend::initialize(const D3D11VideoEncoderDesc& desc) {
    desc_ = desc;
    if (!desc_.input.core) {
        throw D3DVideoEncoderError("NvencD3D11EncoderBackend requires desc.input.core.");
    }
    if (!IsYuv420EncodeFormat(desc_.internalFormat)) {
        throw D3DVideoEncoderError("NvencD3D11EncoderBackend requires internalFormat NV12 or P010.");
    }
    if (desc_.codec == VideoCodec::H264 && desc_.internalFormat != VideoPixelFormat::NV12) {
        throw D3DVideoEncoderError("NvencD3D11 H.264 requires internalFormat=NV12.");
    }

    NvencSessionDesc sessionDesc = {};
    sessionDesc.outputPath = desc_.outputPath;
    sessionDesc.width = desc_.width;
    sessionDesc.height = desc_.height;
    sessionDesc.frameRateNum = desc_.frameRateNum;
    sessionDesc.frameRateDen = desc_.frameRateDen;
    sessionDesc.bitrate = desc_.bitrate;
    sessionDesc.gopLength = desc_.gopLength;
    sessionDesc.bFrameCount = desc_.bFrameCount;
    sessionDesc.codec = desc_.codec;
    sessionDesc.inputFormat = desc_.internalFormat;
    sessionDesc.rateControl = desc_.rateControl;

    log_.info("NvencD3D11EncoderBackend initialize");
    session_.initialize(desc_.input.core->GetDevice(), NV_ENC_DEVICE_TYPE_DIRECTX, sessionDesc);
    open_ = true;
}

void NvencD3D11EncoderBackend::encode(const EncodeSurface& surface, int64_t timestamp100ns, int64_t duration100ns) {
    if (!open_) {
        throw D3DVideoEncoderError("NvencD3D11 encode called after close.");
    }
    if (surface.api != EncodeSurface::Api::D3D11 || !surface.d3d11Texture) {
        throw D3DVideoEncoderError("NvencD3D11 requires a D3D11 encode surface.");
    }
    if (surface.format != ToDxgiFormat(desc_.internalFormat)) {
        throw D3DVideoEncoderError("NvencD3D11 surface format does not match internalFormat.");
    }
    session_.encodeDirectXResource(surface.d3d11Texture.Get(), timestamp100ns, duration100ns);
}

void NvencD3D11EncoderBackend::flush() {
    if (open_) {
        session_.flush();
    }
}

void NvencD3D11EncoderBackend::close() {
    if (!open_) return;
    session_.close();
    open_ = false;
    log_.info("NvencD3D11EncoderBackend closed");
}

} // namespace D3DVideoEncoderLib
