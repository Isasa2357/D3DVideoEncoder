#include "D3D12VideoEncoderImpl.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

#ifdef D3DVIDEOENCODER_HAS_NVENC
#include "backend/nvenc/NvencD3D12EncoderBackend.hpp"
#endif

#include <string>

namespace D3DVideoEncoderLib {

D3D12VideoEncoder::Impl::Impl(const D3D12VideoEncoderDesc& desc)
    : desc_(desc), log_(desc.enableDebugLog) {
    validateDesc();
    timestampGenerator_ = TimestampGenerator(desc_.frameRateNum, desc_.frameRateDen);

    log_.info("D3D12VideoEncoder initialize");
    log_.info(std::string("backend = ") + ToString(desc_.backend));
    log_.info(std::string("codec = ") + ToString(desc_.codec));
    log_.info(std::string("internalFormat = ") + ToString(desc_.internalFormat));

#ifdef D3DVIDEOENCODER_HAS_NVENC
    if (desc_.backend == D3DVideoEncoderBackendType::NvencD3D12) {
        nvencBackend_ = createNvencD3D12Backend();
        nvencBackend_->initialize(desc_);
        open_ = true;
        return;
    }
#endif

    throwBackendNotImplemented();
}

D3D12VideoEncoder::Impl::~Impl() {
    try {
        close();
    } catch (...) {
    }
}

void D3D12VideoEncoder::Impl::validateDesc() const {
    if (desc_.outputPath.empty()) {
        throw D3DVideoEncoderError("D3D12VideoEncoderDesc.outputPath is empty.");
    }
    if (desc_.width == 0 || desc_.height == 0) {
        throw D3DVideoEncoderError("D3D12VideoEncoderDesc width/height must be non-zero.");
    }
    if ((desc_.width % 2) != 0 || (desc_.height % 2) != 0) {
        throw D3DVideoEncoderError("NV12/P010 encoding requires even width and height.");
    }
    if (desc_.frameRateNum == 0 || desc_.frameRateDen == 0) {
        throw D3DVideoEncoderError("frameRateNum and frameRateDen must be non-zero.");
    }
    if (desc_.bitrate == 0) {
        throw D3DVideoEncoderError("bitrate must be non-zero.");
    }
    if (desc_.queueDepth == 0) {
        throw D3DVideoEncoderError("queueDepth must be non-zero.");
    }
    if (!IsYuv420EncodeFormat(desc_.internalFormat)) {
        throw D3DVideoEncoderError("internalFormat must be NV12 or P010.");
    }
    if (desc_.codec == VideoCodec::H264 && desc_.internalFormat != VideoPixelFormat::NV12) {
        throw D3DVideoEncoderError("H.264 path requires internalFormat=NV12.");
    }
    if (!desc_.input.core) {
        throw D3DVideoEncoderError("desc.input.core must be set for D3D12VideoEncoder.");
    }
    if (!IsSupportedD3D12InputFormat(desc_.input.inputFormat)) {
        throw D3DVideoEncoderError("desc.input.inputFormat is unsupported.");
    }
    if (desc_.backend == D3DVideoEncoderBackendType::NvencD3D11) {
        throw D3DVideoEncoderError("NvencD3D11 is not a D3D12VideoEncoder backend.");
    }
}

void D3D12VideoEncoder::Impl::throwBackendNotImplemented() const {
    switch (desc_.backend) {
    case D3DVideoEncoderBackendType::MediaFoundation:
        throw D3DVideoEncoderError(
            "D3D12VideoEncoder MediaFoundation backend is postponed. "
            "Use D3D11VideoEncoder for Media Foundation, or use NvencD3D12 when D3DVIDEOENCODER_ENABLE_NVENC=ON.");
    case D3DVideoEncoderBackendType::NvencD3D12:
        throw D3DVideoEncoderError(
            "D3D12VideoEncoder NvencD3D12 backend requires D3DVIDEOENCODER_ENABLE_NVENC=ON "
            "and a NVIDIA Video Codec SDK include directory.");
    case D3DVideoEncoderBackendType::D3D12VideoEncode:
        throw D3DVideoEncoderError(
            "D3D12VideoEncoder native D3D12 Video Encode backend is planned but not implemented.");
    case D3DVideoEncoderBackendType::NvencD3D11:
        throw D3DVideoEncoderError("NvencD3D11 is not a D3D12VideoEncoder backend.");
    default:
        throw D3DVideoEncoderError("Unsupported D3D12VideoEncoder backend.");
    }
}

#ifdef D3DVIDEOENCODER_HAS_NVENC
std::unique_ptr<INvencD3D12EncoderBackend> D3D12VideoEncoder::Impl::createNvencD3D12Backend() {
    return std::make_unique<NvencD3D12EncoderBackend>(log_);
}
#endif

void D3D12VideoEncoder::Impl::write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState) {
    const int64_t timestamp = timestampGenerator_.nextTimestamp100ns();
    write(resource, currentState, timestamp);
}

void D3D12VideoEncoder::Impl::write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns) {
    if (!open_) {
        throwBackendNotImplemented();
    }
    if (timestamp100ns <= lastTimestamp100ns_) {
        throw D3DVideoEncoderError("timestamp100ns must be strictly increasing.");
    }

#ifdef D3DVIDEOENCODER_HAS_NVENC
    if (nvencBackend_) {
        nvencBackend_->encode(resource, currentState, timestamp100ns, timestampGenerator_.frameDuration100ns());
        lastTimestamp100ns_ = timestamp100ns;
        ++writtenFrameCount_;
        return;
    }
#endif

    throwBackendNotImplemented();
}

void D3D12VideoEncoder::Impl::flush() {
#ifdef D3DVIDEOENCODER_HAS_NVENC
    if (nvencBackend_) {
        nvencBackend_->flush();
    }
#endif
}

void D3D12VideoEncoder::Impl::close() {
    if (!open_) return;
#ifdef D3DVIDEOENCODER_HAS_NVENC
    if (nvencBackend_) {
        nvencBackend_->close();
    }
#endif
    open_ = false;
}

} // namespace D3DVideoEncoderLib
