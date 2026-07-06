#pragma once

#include "D3D11VideoEncoder.hpp"
#include "D3D12VideoEncoder.hpp"
#include "D3DVideoEncoderDesc.hpp"
#include "D3DVideoEncoderError.hpp"

#include <memory>

namespace D3DVideoEncoderLib {

// Backward-compatible migration wrapper.
// New code should use D3D11VideoEncoder or D3D12VideoEncoder directly.
class D3DVideoEncoder {
public:
    explicit D3DVideoEncoder(const D3DVideoEncoderDesc& desc);
    ~D3DVideoEncoder();

    D3DVideoEncoder(const D3DVideoEncoder&) = delete;
    D3DVideoEncoder& operator=(const D3DVideoEncoder&) = delete;

    D3DVideoEncoder(D3DVideoEncoder&&) noexcept;
    D3DVideoEncoder& operator=(D3DVideoEncoder&&) noexcept;

    void write(ID3D11Texture2D* texture);
    void write(ID3D11Texture2D* texture, int64_t timestamp100ns);
    void write(ID3D11Texture2D* texture, int64_t timestamp100ns, int64_t duration100ns);

    void write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState);
    void write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns);
    void write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns, int64_t duration100ns);

    void flush();
    void close();

    bool isOpen() const noexcept;
    uint64_t writtenFrameCount() const noexcept;

private:
    D3DVideoInputApi api_ = D3DVideoInputApi::D3D11;
    std::unique_ptr<D3D11VideoEncoder> d3d11_;
    std::unique_ptr<D3D12VideoEncoder> d3d12_;
};

} // namespace D3DVideoEncoderLib
