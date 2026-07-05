#include "backend/nvenc/NvencD3D12EncoderBackend.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

#include <wrl/client.h>
#include <sstream>

namespace D3DVideoEncoderLib {

NvencD3D12EncoderBackend::NvencD3D12EncoderBackend(DebugLog log) : log_(log) {}
NvencD3D12EncoderBackend::~NvencD3D12EncoderBackend() {
    try { close(); } catch (...) {}
}

void NvencD3D12EncoderBackend::initialize(const D3D12VideoEncoderDesc& desc) {
    desc_ = desc;
    if (!desc_.input.core) {
        throw D3DVideoEncoderError("NvencD3D12EncoderBackend requires desc.input.core.");
    }
    if (!IsYuv420EncodeFormat(desc_.internalFormat)) {
        throw D3DVideoEncoderError("NvencD3D12EncoderBackend requires internalFormat NV12 or P010.");
    }
    if (desc_.input.inputFormat != ToDxgiFormat(desc_.internalFormat)) {
        throw D3DVideoEncoderError(
            "NvencD3D12 currently requires desc.input.inputFormat to already match internalFormat. "
            "D3D12Processing RGB->NV12/P010 integration is a later step.");
    }
    if (desc_.codec == VideoCodec::H264 && desc_.internalFormat != VideoPixelFormat::NV12) {
        throw D3DVideoEncoderError("NvencD3D12 H.264 requires internalFormat=NV12.");
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

    log_.info("NvencD3D12EncoderBackend initialize");
    session_.initialize(desc_.input.core->GetDevice(), NV_ENC_DEVICE_TYPE_DIRECTX, sessionDesc);
    open_ = true;
}

void NvencD3D12EncoderBackend::validateResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState) const {
    if (!resource) {
        throw D3DVideoEncoderError("NvencD3D12 encode received a null ID3D12Resource.");
    }
    if (currentState != D3D12_RESOURCE_STATE_COMMON) {
        throw D3DVideoEncoderError(
            "NvencD3D12 currently requires the input resource state to be D3D12_RESOURCE_STATE_COMMON. "
            "Queue/fence/state transition integration will be added with the D3D12Processing path.");
    }

    const D3D12_RESOURCE_DESC rd = resource->GetDesc();
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        rd.Width != desc_.width ||
        rd.Height != desc_.height ||
        rd.Format != ToDxgiFormat(desc_.internalFormat)) {
        std::ostringstream oss;
        oss << "NvencD3D12 input resource mismatch. expected="
            << desc_.width << "x" << desc_.height
            << " format=" << static_cast<int>(ToDxgiFormat(desc_.internalFormat))
            << " actual=" << static_cast<uint64_t>(rd.Width) << "x" << rd.Height
            << " format=" << static_cast<int>(rd.Format);
        throw D3DVideoEncoderError(oss.str());
    }

    Microsoft::WRL::ComPtr<ID3D12Device> resourceDevice;
    resource->GetDevice(IID_PPV_ARGS(&resourceDevice));
    if (resourceDevice.Get() != desc_.input.core->GetDevice()) {
        throw D3DVideoEncoderError("NvencD3D12 input resource belongs to a different ID3D12Device than desc.input.core.");
    }
}

void NvencD3D12EncoderBackend::encode(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns, int64_t duration100ns) {
    if (!open_) {
        throw D3DVideoEncoderError("NvencD3D12 encode called after close.");
    }
    validateResource(resource, currentState);
    session_.encodeDirectXResource(resource, timestamp100ns, duration100ns);
}

void NvencD3D12EncoderBackend::flush() {
    if (open_) {
        session_.flush();
    }
}

void NvencD3D12EncoderBackend::close() {
    if (!open_) return;
    session_.close();
    open_ = false;
    log_.info("NvencD3D12EncoderBackend closed");
}

} // namespace D3DVideoEncoderLib
