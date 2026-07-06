#include "backend/d3d12video/D3D12VideoEncodeBackend.hpp"

#include "backend/d3d12video/D3D12VideoEncodeCapabilities.hpp"

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
    queryEncodeSupport();

    // Phase 1 intentionally stops after capability/open validation.  No encoder
    // objects, encoder heaps, command lists, bitstream buffers, or metadata buffers
    // are created here yet.  Phase 2 will add H.264/NV12 EncodeFrame and .h264 output.
    open_ = true;
    log_.info("D3D12VideoEncodeBackend Phase 1 capability/open validation succeeded");
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
    if (desc_.input.inputFormat != ToDxgiFormat(desc_.internalFormat)) {
        throw D3DVideoEncoderError(
            "D3D12VideoEncodeBackend Phase 1 requires direct NV12/P010 input. "
            "RGB input through D3D12Processing will be connected after the native encode path is working.");
    }
    if (desc_.asyncMode) {
        throw D3DVideoEncoderError(
            "D3D12VideoEncodeBackend Phase 1 does not support asyncMode yet. "
            "Enable async after Phase 2 EncodeFrame/output is implemented.");
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

    const HRESULT hr3 = desc_.input.core->GetDevice()->QueryInterface(IID_PPV_ARGS(&videoDevice3_));
    if (FAILED(hr3) || !videoDevice3_) {
        std::ostringstream oss;
        oss << "ID3D12VideoDevice3 is not available on this D3D12 device. HRESULT=0x"
            << std::hex << static_cast<unsigned long>(hr3)
            << ". ID3D12VideoDevice3 is required for CreateVideoEncoder in later phases.";
        throw D3DVideoEncoderError(oss.str());
    }
    log_.info("D3D12VideoEncodeBackend found ID3D12VideoDevice and ID3D12VideoDevice3");
}

void D3D12VideoEncodeBackend::queryEncodeSupport() {
    capability_ = QueryD3D12VideoEncodeDeviceSupport(
        desc_.input.core,
        desc_.codec,
        desc_.internalFormat,
        desc_.width,
        desc_.height);

    if (!capability_.supported) {
        std::ostringstream oss;
        oss << "D3D12VideoEncodeBackend capability check failed for codec=" << ToString(desc_.codec)
            << ", format=" << ToString(desc_.internalFormat)
            << ", size=" << desc_.width << "x" << desc_.height
            << ": " << capability_.message;
        throw D3DVideoEncoderError(oss.str());
    }
}

void D3D12VideoEncodeBackend::encode(ID3D12Resource*, D3D12_RESOURCE_STATES, int64_t, int64_t) {
    throw D3DVideoEncoderError(
        "D3D12VideoEncodeBackend::encode is not implemented yet. "
        "Phase 1 only implements capability query and backend open validation; Phase 2 will add H.264/NV12 EncodeFrame and bitstream output.");
}

void D3D12VideoEncodeBackend::flush() {}

void D3D12VideoEncodeBackend::close() {
    open_ = false;
    videoDevice3_.Reset();
    videoDevice_.Reset();
}

} // namespace D3DVideoEncoderLib
