#pragma once

#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>

#include "util/DebugLog.hpp"
#include "util/TimestampGenerator.hpp"

namespace D3DVideoEncoderLib {

class D3D12VideoEncoder::Impl {
public:
    explicit Impl(const D3D12VideoEncoderDesc& desc);

    void write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState);
    void write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns);

    void flush();
    void close();

    bool isOpen() const noexcept { return open_; }
    uint64_t writtenFrameCount() const noexcept { return writtenFrameCount_; }

private:
    void validateDesc() const;
    [[noreturn]] void throwBackendNotImplemented() const;

    D3D12VideoEncoderDesc desc_ = {};
    DebugLog log_;
    TimestampGenerator timestampGenerator_;
    bool open_ = false;
    uint64_t writtenFrameCount_ = 0;
};

} // namespace D3DVideoEncoderLib
