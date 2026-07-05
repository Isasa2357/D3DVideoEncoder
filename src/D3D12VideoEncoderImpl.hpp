#pragma once

#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>

#include "util/DebugLog.hpp"
#include "util/TimestampGenerator.hpp"

#ifdef D3DVIDEOENCODER_HAS_NVENC
#include "backend/nvenc/INvencD3D12EncoderBackend.hpp"
#endif

#include <memory>

namespace D3DVideoEncoderLib {

class D3D12VideoEncoder::Impl {
public:
    explicit Impl(const D3D12VideoEncoderDesc& desc);
    ~Impl();

    void write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState);
    void write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns);

    void flush();
    void close();

    bool isOpen() const noexcept { return open_; }
    uint64_t writtenFrameCount() const noexcept { return writtenFrameCount_; }

private:
    void validateDesc() const;
    void throwBackendNotImplemented() const;
#ifdef D3DVIDEOENCODER_HAS_NVENC
    std::unique_ptr<INvencD3D12EncoderBackend> createNvencD3D12Backend();
#endif

    D3D12VideoEncoderDesc desc_ = {};
    DebugLog log_;
    TimestampGenerator timestampGenerator_;
#ifdef D3DVIDEOENCODER_HAS_NVENC
    std::unique_ptr<INvencD3D12EncoderBackend> nvencBackend_;
#endif
    bool open_ = false;
    uint64_t writtenFrameCount_ = 0;
    int64_t lastTimestamp100ns_ = -1;
};

} // namespace D3DVideoEncoderLib
