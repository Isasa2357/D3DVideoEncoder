#pragma once

#include "D3DVideoEncoderDesc.hpp"
#include "D3DVideoEncoderError.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace D3DVideoEncoderLib {


struct D3D11MediaFoundationFormatCapability {
    VideoCodec codec = VideoCodec::H264;
    VideoPixelFormat inputFormat = VideoPixelFormat::NV12;

    // True when at least one Media Foundation video encoder MFT accepts the
    // requested input format and produces the requested codec.
    bool supported = false;

    // True when at least one hardware encoder MFT is available for the same pair.
    bool hardwareSupported = false;

    uint32_t encoderCount = 0;
    uint32_t hardwareEncoderCount = 0;

    // S_OK means enumeration itself succeeded. supported=false with S_OK means
    // the codec/format pair was queried but no matching encoder was found.
    HRESULT queryHr = S_OK;
    HRESULT hardwareQueryHr = S_OK;

    std::wstring firstEncoderName;
    std::wstring firstHardwareEncoderName;
};

struct D3D11MediaFoundationCapabilities {
    D3D11MediaFoundationFormatCapability h264Nv12;
    D3D11MediaFoundationFormatCapability hevcNv12;
    D3D11MediaFoundationFormatCapability hevcP010;
    D3D11MediaFoundationFormatCapability av1Nv12;
    D3D11MediaFoundationFormatCapability av1P010;

    bool supportsH264Nv12() const noexcept { return h264Nv12.supported; }
    bool supportsHevcNv12() const noexcept { return hevcNv12.supported; }
    bool supportsHevcP010() const noexcept { return hevcP010.supported; }
    bool supportsAv1Nv12() const noexcept { return av1Nv12.supported; }
    bool supportsAv1P010() const noexcept { return av1P010.supported; }
};

class D3D11VideoEncoder {
public:
    explicit D3D11VideoEncoder(const D3D11VideoEncoderDesc& desc);
    ~D3D11VideoEncoder();

    D3D11VideoEncoder(const D3D11VideoEncoder&) = delete;
    D3D11VideoEncoder& operator=(const D3D11VideoEncoder&) = delete;

    D3D11VideoEncoder(D3D11VideoEncoder&&) noexcept;
    D3D11VideoEncoder& operator=(D3D11VideoEncoder&&) noexcept;

    void write(ID3D11Texture2D* texture);
    void write(ID3D11Texture2D* texture, int64_t timestamp100ns);

    void flush();
    void close();

    bool isOpen() const noexcept;
    uint64_t writtenFrameCount() const noexcept;

    // Query installed Media Foundation video encoder MFTs without creating an encoder.
    // HEVC/P010 support is optional on Windows installations, so callers should use
    // this before selecting codec=HEVC with internalFormat=P010.
    static D3D11MediaFoundationFormatCapability QueryMediaFoundationSupport(
        VideoCodec codec,
        VideoPixelFormat inputFormat);
    static D3D11MediaFoundationCapabilities QueryMediaFoundationCapabilities();

    // Query NVENC support on the D3D11 device. This works only when the library
    // was built with D3DVIDEOENCODER_ENABLE_NVENC=ON; otherwise supported=false
    // is returned with an explanatory message.
    static NvencFormatCapability QueryNvencSupport(
        D3D11CoreLib::D3D11Core* core,
        VideoCodec codec,
        VideoPixelFormat inputFormat);
    static NvencCapabilities QueryNvencCapabilities(D3D11CoreLib::D3D11Core* core);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace D3DVideoEncoderLib
