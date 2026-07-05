#include "backend/MediaFoundationRuntime.hpp"

#include "util/HResult.hpp"

#include <mfapi.h>

namespace D3DVideoEncoderLib {

std::mutex MediaFoundationRuntime::mutex_;
unsigned int MediaFoundationRuntime::refCount_ = 0;

MediaFoundationRuntime::~MediaFoundationRuntime() {
    shutdown();
}

void MediaFoundationRuntime::startup() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) return;

    if (refCount_ == 0) {
        D3DVE_THROW_IF_FAILED(MFStartup(MF_VERSION));
    }
    ++refCount_;
    started_ = true;
}

void MediaFoundationRuntime::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_) return;

    if (refCount_ > 0) {
        --refCount_;
        if (refCount_ == 0) {
            MFShutdown();
        }
    }
    started_ = false;
}

} // namespace D3DVideoEncoderLib
