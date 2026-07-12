#include "D3D11VideoEncoderImpl.hpp"

#include "backend/MediaFoundationEncoderBackend.hpp"
#include "backend/UnsupportedEncoderBackend.hpp"

#ifdef D3DVIDEOENCODER_HAS_NVENC
#include "backend/nvenc/NvencD3D11EncoderBackend.hpp"
#endif

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

#include <cstddef>
#include <d3d11_4.h>
#include <sstream>
#include <utility>
#include <wrl/client.h>

namespace D3DVideoEncoderLib {

D3D11VideoEncoder::Impl::Impl(const D3D11VideoEncoderDesc& desc)
    : desc_(desc), log_(desc.enableDebugLog) {

    validateDesc();
    timestampGenerator_ = TimestampGenerator(desc_.frameRateNum, desc_.frameRateDen);

    log_.info("D3D11VideoEncoder initialize");
    log_.info(std::string("backend = ") + ToString(desc_.backend));
    log_.info(std::string("codec = ") + ToString(desc_.codec));
    log_.info(std::string("internalFormat = ") + ToString(desc_.internalFormat));

    enableMultithreadProtectionIfNeeded();
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
        throw D3DVideoEncoderError("D3D11VideoEncoder supports MediaFoundation and NvencD3D11 backends.");
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
#ifdef D3DVIDEOENCODER_HAS_NVENC
        return std::make_unique<NvencD3D11EncoderBackend>(log_);
#else
        return std::make_unique<UnsupportedEncoderBackend>(desc_.backend, log_);
#endif
    default:
        throw D3DVideoEncoderError("Unsupported D3D11 encoder backend.");
    }
}

void D3D11VideoEncoder::Impl::enableMultithreadProtectionIfNeeded() {
    if (!desc_.asyncMode || !desc_.input.core || !desc_.input.core->GetImmediateContext()) {
        return;
    }

    Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
    const HRESULT hr = desc_.input.core->GetImmediateContext()->QueryInterface(IID_PPV_ARGS(&multithread));
    if (SUCCEEDED(hr) && multithread) {
        multithread->SetMultithreadProtected(TRUE);
        log_.info("D3D11VideoEncoder enabled ID3D11Multithread protection for async mode");
    } else {
        log_.info("D3D11VideoEncoder could not enable ID3D11Multithread protection for async mode");
    }
}

void D3D11VideoEncoder::Impl::write(ID3D11Texture2D* texture) {
    const int64_t timestamp = timestampGenerator_.nextTimestamp100ns();
    write(texture, timestamp);
}

void D3D11VideoEncoder::Impl::write(ID3D11Texture2D* texture, int64_t timestamp100ns) {
    write(texture, timestamp100ns, timestampGenerator_.frameDuration100ns());
}

void D3D11VideoEncoder::Impl::write(ID3D11Texture2D* texture, int64_t timestamp100ns, int64_t duration100ns) {
    if (!open_) {
        throw D3DVideoEncoderError("write() called after close().");
    }
    throwWorkerExceptionIfSet();
    if (timestamp100ns <= lastTimestamp100ns_) {
        throw D3DVideoEncoderError("timestamp100ns must be strictly increasing.");
    }
    if (duration100ns <= 0) {
        throw D3DVideoEncoderError("duration100ns must be positive.");
    }

    EncodeSurface surface = input_.prepare(texture);
    encodeOrQueue(surface, timestamp100ns, duration100ns);
    lastTimestamp100ns_ = timestamp100ns;
    ++writtenFrameCount_;
}

void D3D11VideoEncoder::Impl::encodeOrQueue(EncodeSurface surface, int64_t timestamp100ns, int64_t duration100ns) {
    if (surface.api != backend_->requiredSurfaceApi() || surface.format != ToDxgiFormat(backend_->requiredInputFormat())) {
        releaseSurface(surface);
        throw D3DVideoEncoderError("Input adapter produced a surface that does not match backend requirements.");
    }

    if (!desc_.asyncMode) {
        try {
            encodeNow(surface, timestamp100ns, duration100ns);
        } catch (...) {
            releaseSurface(surface);
            throw;
        }
        releaseSurface(surface);
        return;
    }

    EncodeJob job;
    job.surface = surface;
    job.timestamp100ns = timestamp100ns;
    job.duration100ns = duration100ns;

    EncodeJob droppedJob;
    try {
        if (!jobQueue_.push(std::move(job), &droppedJob)) {
            releaseSurface(surface);
        } else if (droppedJob.surface.poolIndex != static_cast<std::size_t>(-1)) {
            releaseSurface(droppedJob.surface);
        }
    } catch (...) {
        releaseSurface(surface);
        if (droppedJob.surface.poolIndex != static_cast<std::size_t>(-1)) {
            releaseSurface(droppedJob.surface);
        }
        throw;
    }

    throwWorkerExceptionIfSet();
}

void D3D11VideoEncoder::Impl::encodeNow(const EncodeSurface& surface, int64_t timestamp100ns, int64_t duration100ns) {
    backend_->encode(surface, timestamp100ns, duration100ns);
}

void D3D11VideoEncoder::Impl::releaseSurface(const EncodeSurface& surface) {
    input_.release(surface);
}

void D3D11VideoEncoder::Impl::releasePendingJobs(std::vector<EncodeJob>& jobs) {
    for (auto& job : jobs) {
        releaseSurface(job.surface);
    }
    jobs.clear();
}

void D3D11VideoEncoder::Impl::throwWorkerExceptionIfSet() {
    if (workerErrorSet_.load()) {
        std::rethrow_exception(workerException_);
    }
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
        auto pending = jobQueue_.cancelPending();
        releasePendingJobs(pending);
    }
}

void D3D11VideoEncoder::Impl::stopWorker() {
    if (!desc_.asyncMode) return;

    if (workerErrorSet_.load()) {
        auto pending = jobQueue_.cancelPending();
        releasePendingJobs(pending);
    } else {
        jobQueue_.waitDrained();
        jobQueue_.close();
    }

    if (worker_.joinable()) {
        worker_.join();
    }
    throwWorkerExceptionIfSet();
}

void D3D11VideoEncoder::Impl::flush() {
    if (!open_) return;
    throwWorkerExceptionIfSet();
    if (desc_.asyncMode) {
        jobQueue_.waitDrained();
        throwWorkerExceptionIfSet();
    }
    input_.waitAllSurfacesFree();
    input_.flush();
    backend_->flush();
}

void D3D11VideoEncoder::Impl::close() {
    if (!open_) return;

    std::exception_ptr pendingException = nullptr;

    try {
        stopWorker();
    } catch (...) {
        pendingException = std::current_exception();
    }

    try {
        input_.waitAllSurfacesFree();
        input_.flush();
        backend_->close();
    } catch (...) {
        if (!pendingException) {
            pendingException = std::current_exception();
        }
    }

    open_ = false;
    log_.info("D3D11VideoEncoder closed");

    if (pendingException) {
        std::rethrow_exception(pendingException);
    }
}

} // namespace D3DVideoEncoderLib
