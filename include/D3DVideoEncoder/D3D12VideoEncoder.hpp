#pragma once

#include "D3DVideoEncoderDesc.hpp"
#include "D3DVideoEncoderError.hpp"

#include <memory>

namespace D3DVideoEncoderLib {

class D3D12VideoEncoder {
public:
    // This class is intentionally separated from D3D11VideoEncoder.
    // Current package exposes validation and explicit unsupported-backend errors only.
    // Actual D3D12 encoding will be implemented through NvencD3D12 or native D3D12 Video Encode.
    explicit D3D12VideoEncoder(const D3D12VideoEncoderDesc& desc);
    ~D3D12VideoEncoder();

    D3D12VideoEncoder(const D3D12VideoEncoder&) = delete;
    D3D12VideoEncoder& operator=(const D3D12VideoEncoder&) = delete;

    D3D12VideoEncoder(D3D12VideoEncoder&&) noexcept;
    D3D12VideoEncoder& operator=(D3D12VideoEncoder&&) noexcept;

    void write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState);
    void write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns);

    void flush();
    void close();

    bool isOpen() const noexcept;
    uint64_t writtenFrameCount() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace D3DVideoEncoderLib
