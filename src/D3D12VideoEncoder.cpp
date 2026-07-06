#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>

#include "D3D12VideoEncoderImpl.hpp"

#ifdef D3DVIDEOENCODER_HAS_NVENC
#include "backend/nvenc/NvencCommon.hpp"
#endif
#ifdef D3DVIDEOENCODER_HAS_D3D12_VIDEO_ENCODE
#include "backend/d3d12video/D3D12VideoEncodeCapabilities.hpp"
#endif

namespace D3DVideoEncoderLib {

D3D12VideoEncoder::D3D12VideoEncoder(const D3D12VideoEncoderDesc& desc)
    : impl_(std::make_unique<Impl>(desc)) {}

D3D12VideoEncoder::~D3D12VideoEncoder() = default;
D3D12VideoEncoder::D3D12VideoEncoder(D3D12VideoEncoder&&) noexcept = default;
D3D12VideoEncoder& D3D12VideoEncoder::operator=(D3D12VideoEncoder&&) noexcept = default;

void D3D12VideoEncoder::write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState) {
    impl_->write(resource, currentState);
}

void D3D12VideoEncoder::write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns) {
    impl_->write(resource, currentState, timestamp100ns);
}

void D3D12VideoEncoder::write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns, int64_t duration100ns) {
    impl_->write(resource, currentState, timestamp100ns, duration100ns);
}

void D3D12VideoEncoder::flush() {
    impl_->flush();
}

void D3D12VideoEncoder::close() {
    impl_->close();
}

bool D3D12VideoEncoder::isOpen() const noexcept {
    return impl_ && impl_->isOpen();
}

uint64_t D3D12VideoEncoder::writtenFrameCount() const noexcept {
    return impl_ ? impl_->writtenFrameCount() : 0;
}


NvencFormatCapability D3D12VideoEncoder::QueryNvencSupport(
    D3D12CoreLib::D3D12Core* core,
    VideoCodec codec,
    VideoPixelFormat inputFormat) {

    NvencFormatCapability result;
    result.codec = codec;
    result.inputFormat = inputFormat;
#ifdef D3DVIDEOENCODER_HAS_NVENC
    if (!core) {
        result.message = "D3D12VideoEncoder::QueryNvencSupport requires a non-null D3D12Core.";
        return result;
    }
    return QueryNvencDeviceSupport(core->GetDevice(), NV_ENC_DEVICE_TYPE_DIRECTX, codec, inputFormat);
#else
    result.message = "D3DVideoEncoder was built without D3DVIDEOENCODER_ENABLE_NVENC=ON.";
    return result;
#endif
}

NvencCapabilities D3D12VideoEncoder::QueryNvencCapabilities(D3D12CoreLib::D3D12Core* core) {
    NvencCapabilities caps;
    caps.h264Nv12 = QueryNvencSupport(core, VideoCodec::H264, VideoPixelFormat::NV12);
    caps.hevcNv12 = QueryNvencSupport(core, VideoCodec::HEVC, VideoPixelFormat::NV12);
    caps.hevcP010 = QueryNvencSupport(core, VideoCodec::HEVC, VideoPixelFormat::P010);
    caps.av1Nv12 = QueryNvencSupport(core, VideoCodec::AV1, VideoPixelFormat::NV12);
    caps.av1P010 = QueryNvencSupport(core, VideoCodec::AV1, VideoPixelFormat::P010);
    return caps;
}

D3D12VideoEncodeFormatCapability D3D12VideoEncoder::QueryD3D12VideoEncodeSupport(
    D3D12CoreLib::D3D12Core* core,
    VideoCodec codec,
    VideoPixelFormat inputFormat,
    uint32_t width,
    uint32_t height) {

    D3D12VideoEncodeFormatCapability result;
    result.codec = codec;
    result.inputFormat = inputFormat;
    result.requestedWidth = width;
    result.requestedHeight = height;
#ifdef D3DVIDEOENCODER_HAS_D3D12_VIDEO_ENCODE
    return QueryD3D12VideoEncodeDeviceSupport(core, codec, inputFormat, width, height);
#else
    result.message = "D3DVideoEncoder was built without D3DVIDEOENCODER_ENABLE_D3D12_VIDEO_ENCODE=ON.";
    return result;
#endif
}

D3D12VideoEncodeCapabilities D3D12VideoEncoder::QueryD3D12VideoEncodeCapabilities(
    D3D12CoreLib::D3D12Core* core,
    uint32_t width,
    uint32_t height) {

#ifdef D3DVIDEOENCODER_HAS_D3D12_VIDEO_ENCODE
    return QueryD3D12VideoEncodeDeviceCapabilities(core, width, height);
#else
    D3D12VideoEncodeCapabilities caps;
    caps.h264Nv12 = QueryD3D12VideoEncodeSupport(core, VideoCodec::H264, VideoPixelFormat::NV12, width, height);
    caps.hevcNv12 = QueryD3D12VideoEncodeSupport(core, VideoCodec::HEVC, VideoPixelFormat::NV12, width, height);
    caps.hevcP010 = QueryD3D12VideoEncodeSupport(core, VideoCodec::HEVC, VideoPixelFormat::P010, width, height);
    return caps;
#endif
}

} // namespace D3DVideoEncoderLib
