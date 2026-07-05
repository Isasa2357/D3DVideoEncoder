#pragma once

#include <D3DVideoEncoder/D3D11VideoEncoder.hpp>

#include "async/EncodeJobQueue.hpp"
#include "backend/IVideoEncoderBackend.hpp"
#include "input/D3D11VideoInputAdapter.hpp"
#include "util/DebugLog.hpp"
#include "util/TimestampGenerator.hpp"

#include <atomic>
#include <memory>
#include <thread>

namespace D3DVideoEncoderLib {

class D3D11VideoEncoder::Impl {
public:
    explicit Impl(const D3D11VideoEncoderDesc& desc);
    ~Impl();

    void write(ID3D11Texture2D* texture);
    void write(ID3D11Texture2D* texture, int64_t timestamp100ns);

    void flush();
    void close();

    bool isOpen() const noexcept { return open_; }
    uint64_t writtenFrameCount() const noexcept { return writtenFrameCount_; }

private:
    void validateDesc() const;
    std::unique_ptr<IVideoEncoderBackend> createBackend();

    void encodeOrQueue(EncodeSurface surface, int64_t timestamp100ns, int64_t duration100ns);
    void encodeNow(const EncodeSurface& surface, int64_t timestamp100ns, int64_t duration100ns);
    void releaseSurface(const EncodeSurface& surface);
    void startWorkerIfNeeded();
    void workerMain();
    void stopWorker();

    D3D11VideoEncoderDesc desc_ = {};
    DebugLog log_;
    TimestampGenerator timestampGenerator_;

    D3D11VideoInputAdapter input_;
    std::unique_ptr<IVideoEncoderBackend> backend_;

    EncodeJobQueue jobQueue_;
    std::thread worker_;
    std::atomic<bool> workerErrorSet_{false};
    std::exception_ptr workerException_ = nullptr;

    bool open_ = false;
    uint64_t writtenFrameCount_ = 0;
    int64_t lastTimestamp100ns_ = -1;
};

} // namespace D3DVideoEncoderLib
