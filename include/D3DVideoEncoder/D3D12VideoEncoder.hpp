#pragma once

#include "D3DVideoEncoderDesc.hpp"
#include "D3DVideoEncoderError.hpp"

#include <memory>

namespace D3DVideoEncoderLib {

class D3D12VideoEncoder {
public:
    // This class is intentionally separated from D3D11VideoEncoder.
    // D3D12 output is handled by D3D12-native backends. NvencD3D12 can encode
    // NV12/P010 directly and can optionally use D3D12Processing for RGB -> NV12/P010.
    // Native D3D12 Video Encode is exposed as an experimental backend scaffold.
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

    // Query NVENC support on the D3D12 device. This works only when the library
    // was built with D3DVIDEOENCODER_ENABLE_NVENC=ON; otherwise supported=false
    // is returned with an explanatory message.
    static NvencFormatCapability QueryNvencSupport(
        D3D12CoreLib::D3D12Core* core,
        VideoCodec codec,
        VideoPixelFormat inputFormat);
    static NvencCapabilities QueryNvencCapabilities(D3D12CoreLib::D3D12Core* core);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace D3DVideoEncoderLib
