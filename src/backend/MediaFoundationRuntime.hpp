#pragma once

#include <mutex>

namespace D3DVideoEncoderLib {

class MediaFoundationRuntime {
public:
    MediaFoundationRuntime() = default;
    ~MediaFoundationRuntime();

    MediaFoundationRuntime(const MediaFoundationRuntime&) = delete;
    MediaFoundationRuntime& operator=(const MediaFoundationRuntime&) = delete;

    void startup();
    void shutdown();

private:
    bool started_ = false;

    static std::mutex mutex_;
    static unsigned int refCount_;
};

} // namespace D3DVideoEncoderLib
