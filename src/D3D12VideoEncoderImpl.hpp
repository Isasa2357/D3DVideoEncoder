#pragma once

#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>

#include "util/DebugLog.hpp"
#include "util/TimestampGenerator.hpp"
#include "async/D3D12EncodeJobQueue.hpp"

#ifdef D3DVIDEOENCODER_HAS_NVENC
#include "backend/nvenc/INvencD3D12EncoderBackend.hpp"
#endif
#ifdef D3DVIDEOENCODER_HAS_D3D12_VIDEO_ENCODE
#include "backend/d3d12video/ID3D12VideoEncodeBackend.hpp"
#endif

#include <atomic>
#include <exception>
#include <memory>
#include <thread>
#include <vector>

namespace D3DVideoEncoderLib {

class D3D12VideoEncoder::Impl {
public:
    explicit Impl(const D3D12VideoEncoderDesc& desc);
    ~Impl();

    void write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState);
    void write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns);
    void write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns, int64_t duration100ns);

    void flush();
    void close();

    bool isOpen() const noexcept { return open_; }
    uint64_t writtenFrameCount() const noexcept { return writtenFrameCount_; }

private:
    void validateDesc() const;
    void throwBackendNotImplemented() const;
    void encodeNow(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns, int64_t duration100ns);
    void encodeOrQueue(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns, int64_t duration100ns);
    void startWorkerIfNeeded();
    void workerMain();
    void stopWorker();
    void throwWorkerExceptionIfSet();
    void releasePendingJobs(std::vector<D3D12EncodeJob>& jobs);
#ifdef D3DVIDEOENCODER_HAS_NVENC
    std::unique_ptr<INvencD3D12EncoderBackend> createNvencD3D12Backend();
#endif
#ifdef D3DVIDEOENCODER_HAS_D3D12_VIDEO_ENCODE
    std::unique_ptr<ID3D12VideoEncodeBackend> createD3D12VideoEncodeBackend();
#endif

    D3D12VideoEncoderDesc desc_ = {};
    DebugLog log_;
    TimestampGenerator timestampGenerator_;
#ifdef D3DVIDEOENCODER_HAS_NVENC
    std::unique_ptr<INvencD3D12EncoderBackend> nvencBackend_;
#endif
#ifdef D3DVIDEOENCODER_HAS_D3D12_VIDEO_ENCODE
    std::unique_ptr<ID3D12VideoEncodeBackend> d3d12VideoEncodeBackend_;
#endif
    D3D12EncodeJobQueue jobQueue_;
    std::thread worker_;
    std::exception_ptr workerException_ = nullptr;
    std::atomic<bool> workerErrorSet_{ false };
    bool open_ = false;
    uint64_t writtenFrameCount_ = 0;
    int64_t lastTimestamp100ns_ = -1;
};

} // namespace D3DVideoEncoderLib
