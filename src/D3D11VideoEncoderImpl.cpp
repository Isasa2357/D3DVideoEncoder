#include "D3D11VideoEncoderImpl.hpp"

#include "backend/MediaFoundationEncoderBackend.hpp"
#include "backend/UnsupportedEncoderBackend.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

#include <sstream>
#include <utility>

namespace D3DVideoEncoderLib {

D3D11VideoEncoder::Impl::Impl(const D3D11VideoEncoderDesc& desc)
    : desc_(desc), log_(desc.enableDebugLog) {

    validateDesc();
    timestampGenerator_ = TimestampGenerator(desc_.frameRateNum, desc_.frameRateDen);

    log_.info("D3D11VideoEncoder initialize");
    log_.info(std::string("backend = ") + ToString(desc_.backend));
    log_.info(std::string("codec = ") + ToString(desc_.codec));
    log_.info(std::string("internalFormat = ") + ToString(desc_.internalFormat));

    input_.initialize(desc_, log_);

    backend_ = createBackend();
    backend_->initialize(desc_);

    if (desc_.asyncMode) {
        jobQueue_.initialize(desc_.queueDepth, desc_.queueFullPolicy);
        startWorkerIfNeeded();
    }

    open_ = true;
}

D3D11VideoEncoder::Impl::~Impl() {
    try {
        close();
    } catch (...) {
    }
}

void D3D11VideoEncoder::Impl::validateDesc() const {
    if (desc_.outputPath.empty()) {
        throw D3DVideoEncoderError("D3D11VideoEncoderDesc.outputPath is empty.");
    }
    if (desc_.width == 0 || desc_.height == 0) {
        throw D3DVideoEncoderError("D3D11VideoEncoderDesc width/height must be non-zero.");
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
    if (desc_.backend != D3DVideoEncoderBackendType::MediaFoundation &&
        desc_.backend != D3DVideoEncoderBackendType::NvencD3D11) {
        throw D3DVideoEncoderError("D3D11VideoEncoder supports MediaFoundation now; NvencD3D11 is reserved for future implementation.");
    }
    if (desc_.backend == D3DVideoEncoderBackendType::MediaFoundation && desc_.codec == VideoCodec::AV1) {
        throw D3DVideoEncoderError("MediaFoundation AV1 backend is not enabled. Use NVENC D3D11 when available.");
    }
    if (!desc_.input.core) {
        throw D3DVideoEncoderError("desc.input.core must be set for D3D11VideoEncoder.");
    }
    if (!IsSupportedD3D11InputFormat(desc_.input.inputFormat)) {
        throw D3DVideoEncoderError("desc.input.inputFormat is unsupported.");
    }
}

std::unique_ptr<IVideoEncoderBackend> D3D11VideoEncoder::Impl::createBackend() {
    switch (desc_.backend) {
    case D3DVideoEncoderBackendType::MediaFoundation:
        return std::make_unique<MediaFoundationEncoderBackend>(log_);
    case D3DVideoEncoderBackendType::NvencD3D11:
        return std::make_unique<UnsupportedEncoderBackend>(desc_.backend, log_);
    default:
        throw D3DVideoEncoderError("Unsupported D3D11 encoder backend.");
    }
}

void D3D11VideoEncoder::Impl::write(ID3D11Texture2D* texture) {
    const int64_t timestamp = timestampGenerator_.nextTimestamp100ns();
    write(texture, timestamp);
}

void D3D11VideoEncoder::Impl::write(ID3D11Texture2D* texture, int64_t timestamp100ns) {
    if (!open_) {
        throw D3DVideoEncoderError("write() called after close().");
    }
    if (timestamp100ns <= lastTimestamp100ns_) {
        throw D3DVideoEncoderError("timestamp100ns must be strictly increasing.");
    }

    EncodeSurface surface = input_.prepare(texture);
    encodeOrQueue(surface, timestamp100ns, timestampGenerator_.frameDuration100ns());
    lastTimestamp100ns_ = timestamp100ns;
    ++writtenFrameCount_;
}

void D3D11VideoEncoder::Impl::encodeOrQueue(EncodeSurface surface, int64_t timestamp100ns, int64_t duration100ns) {
    if (surface.api != backend_->requiredSurfaceApi() || surface.format != ToDxgiFormat(backend_->requiredInputFormat())) {
        releaseSurface(surface);
        throw D3DVideoEncoderError("Input adapter produced a surface that does not match backend requirements.");
    }

    if (!desc_.asyncMode) {
        encodeNow(surface, timestamp100ns, duration100ns);
        releaseSurface(surface);
        return;
    }

    EncodeJob job;
    job.surface = surface;
    job.timestamp100ns = timestamp100ns;
    job.duration100ns = duration100ns;
    if (!jobQueue_.push(std::move(job))) {
        releaseSurface(surface);
    }

    if (workerErrorSet_.load()) {
        std::rethrow_exception(workerException_);
    }
}

void D3D11VideoEncoder::Impl::encodeNow(const EncodeSurface& surface, int64_t timestamp100ns, int64_t duration100ns) {
    backend_->encode(surface, timestamp100ns, duration100ns);
}

void D3D11VideoEncoder::Impl::releaseSurface(const EncodeSurface& surface) {
    input_.release(surface);
}

void D3D11VideoEncoder::Impl::startWorkerIfNeeded() {
    worker_ = std::thread([this]() { workerMain(); });
}

void D3D11VideoEncoder::Impl::workerMain() {
    try {
        EncodeJob job;
        while (jobQueue_.pop(job)) {
            try {
                backend_->encode(job.surface, job.timestamp100ns, job.duration100ns);
            } catch (...) {
                releaseSurface(job.surface);
                jobQueue_.jobDone();
                throw;
            }
            releaseSurface(job.surface);
            jobQueue_.jobDone();
        }
    } catch (...) {
        workerException_ = std::current_exception();
        workerErrorSet_.store(true);
    }
}

void D3D11VideoEncoder::Impl::stopWorker() {
    if (!desc_.asyncMode) return;
    jobQueue_.waitDrained();
    jobQueue_.close();
    if (worker_.joinable()) {
        worker_.join();
    }
    if (workerErrorSet_.load()) {
        std::rethrow_exception(workerException_);
    }
}

void D3D11VideoEncoder::Impl::flush() {
    if (!open_) return;
    if (desc_.asyncMode) {
        jobQueue_.waitDrained();
    }
    input_.flush();
    backend_->flush();
}

void D3D11VideoEncoder::Impl::close() {
    if (!open_) return;

    stopWorker();
    input_.flush();
    backend_->close();
    open_ = false;
    log_.info("D3D11VideoEncoder closed");
}

} // namespace D3DVideoEncoderLib
