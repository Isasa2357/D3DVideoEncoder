#include "backend/d3d12video/D3D12VideoEncodeBackend.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

#include <d3d12video.h>
#include <sstream>

namespace D3DVideoEncoderLib {

D3D12VideoEncodeBackend::D3D12VideoEncodeBackend(DebugLog log) : log_(log) {}
D3D12VideoEncodeBackend::~D3D12VideoEncodeBackend() {
    try { close(); } catch (...) {}
}

void D3D12VideoEncodeBackend::initialize(const D3D12VideoEncoderDesc& desc) {
    desc_ = desc;
    validateDesc();
    queryVideoDevice();

    // This backend intentionally stops before creating encoder heaps/bitstream buffers.
    // D3D12 Video Encode requires codec-specific rate-control, picture-control,
    // resolved metadata, and external mux/bitstream plumbing.  Keeping this as a
    // capability/open scaffold prevents callers from accidentally assuming that a
    // complete native path exists while still giving a concrete compile-time entry
    // point for the next implementation pass.
    throw D3DVideoEncoderError(
        "D3D12VideoEncodeBackend is available as a build/capability scaffold, but frame encoding and bitstream output are not implemented yet. "
        "Use NvencD3D12 for D3D12 output today, or keep backend=D3D12VideoEncode for future native D3D12 Video Encode work.");
}

void D3D12VideoEncodeBackend::validateDesc() const {
    if (!desc_.input.core) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend requires desc.input.core.");
    }
    if (!IsYuv420EncodeFormat(desc_.internalFormat)) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend requires internalFormat NV12 or P010.");
    }
    if (desc_.codec == VideoCodec::H264 && desc_.internalFormat != VideoPixelFormat::NV12) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend H.264 requires internalFormat=NV12.");
    }
    if (!IsSupportedD3D12InputFormat(desc_.input.inputFormat)) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend input.inputFormat is unsupported.");
    }
}

void D3D12VideoEncodeBackend::queryVideoDevice() {
    const HRESULT hr = desc_.input.core->GetDevice()->QueryInterface(IID_PPV_ARGS(&videoDevice_));
    if (FAILED(hr) || !videoDevice_) {
        std::ostringstream oss;
        oss << "ID3D12VideoDevice is not available on this D3D12 device. HRESULT=0x"
            << std::hex << static_cast<unsigned long>(hr);
        throw D3DVideoEncoderError(oss.str());
    }
    log_.info("D3D12VideoEncodeBackend found ID3D12VideoDevice");
}

void D3D12VideoEncodeBackend::encode(ID3D12Resource*, D3D12_RESOURCE_STATES, int64_t, int64_t) {
    throw D3DVideoEncoderError("D3D12VideoEncodeBackend::encode is not implemented yet.");
}

void D3D12VideoEncodeBackend::flush() {}

void D3D12VideoEncodeBackend::close() {
    open_ = false;
    videoDevice_.Reset();
}

} // namespace D3DVideoEncoderLib
