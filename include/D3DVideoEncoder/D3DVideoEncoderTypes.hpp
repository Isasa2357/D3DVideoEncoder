#pragma once

#include <cstdint>
#include <string>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_6.h>

namespace D3DVideoEncoderLib {

enum class D3DVideoInputApi {
    D3D11,
    D3D12,
};

enum class D3DVideoEncoderBackendType {
    MediaFoundation,
    NvencD3D11,
    NvencD3D12,
    D3D12VideoEncode,
};

enum class VideoCodec {
    H264,
    HEVC,
    AV1,
};

enum class VideoRateControlMode {
    CBR,
    VBR,
    ConstantQP,
    Quality,
};

enum class VideoPixelFormat {
    NV12,
    P010,
    BGRA8,
    RGBA8,
    RGBA16F,
};

enum class VideoColorRange {
    Limited,
    Full,
};

enum class VideoColorMatrix {
    BT601,
    BT709,
    BT2020,
};

enum class EncoderQueueFullPolicy {
    Block,
    DropNewest,
    DropOldest,
};


struct NvencFormatCapability {
    VideoCodec codec = VideoCodec::H264;
    VideoPixelFormat inputFormat = VideoPixelFormat::NV12;

    bool runtimeAvailable = false;
    bool deviceSupported = false;
    bool codecSupported = false;
    bool inputFormatSupported = false;
    bool supported = false;

    uint32_t maxWidth = 0;
    uint32_t maxHeight = 0;

    std::string message;

    bool canEncode() const noexcept { return supported; }
};

struct NvencCapabilities {
    NvencFormatCapability h264Nv12;
    NvencFormatCapability hevcNv12;
    NvencFormatCapability hevcP010;
    NvencFormatCapability av1Nv12;
    NvencFormatCapability av1P010;

    bool supportsH264Nv12() const noexcept { return h264Nv12.supported; }
    bool supportsHevcNv12() const noexcept { return hevcNv12.supported; }
    bool supportsHevcP010() const noexcept { return hevcP010.supported; }
    bool supportsAv1Nv12() const noexcept { return av1Nv12.supported; }
    bool supportsAv1P010() const noexcept { return av1P010.supported; }
};

inline DXGI_FORMAT ToDxgiFormat(VideoPixelFormat format) noexcept {
    switch (format) {
    case VideoPixelFormat::NV12:    return DXGI_FORMAT_NV12;
    case VideoPixelFormat::P010:    return DXGI_FORMAT_P010;
    case VideoPixelFormat::BGRA8:   return DXGI_FORMAT_B8G8R8A8_UNORM;
    case VideoPixelFormat::RGBA8:   return DXGI_FORMAT_R8G8B8A8_UNORM;
    case VideoPixelFormat::RGBA16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:                        return DXGI_FORMAT_UNKNOWN;
    }
}

inline VideoPixelFormat FromDxgiFormat(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_NV12: return VideoPixelFormat::NV12;
    case DXGI_FORMAT_P010: return VideoPixelFormat::P010;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return VideoPixelFormat::BGRA8;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return VideoPixelFormat::RGBA8;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return VideoPixelFormat::RGBA16F;
    default: return VideoPixelFormat::NV12;
    }
}

inline bool IsSupportedD3D11InputFormat(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_NV12 ||
           format == DXGI_FORMAT_P010 ||
           format == DXGI_FORMAT_B8G8R8A8_UNORM ||
           format == DXGI_FORMAT_R8G8B8A8_UNORM ||
           format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

inline bool IsSupportedD3D12InputFormat(DXGI_FORMAT format) noexcept {
    return IsSupportedD3D11InputFormat(format);
}

inline bool IsYuv420EncodeFormat(VideoPixelFormat format) noexcept {
    return format == VideoPixelFormat::NV12 || format == VideoPixelFormat::P010;
}

inline bool IsYuv420DxgiFormat(DXGI_FORMAT format) noexcept {
    return format == DXGI_FORMAT_NV12 || format == DXGI_FORMAT_P010;
}

inline const char* ToString(D3DVideoInputApi api) noexcept {
    switch (api) {
    case D3DVideoInputApi::D3D11: return "D3D11";
    case D3DVideoInputApi::D3D12: return "D3D12";
    default: return "Unknown";
    }
}

inline const char* ToString(D3DVideoEncoderBackendType backend) noexcept {
    switch (backend) {
    case D3DVideoEncoderBackendType::MediaFoundation: return "MediaFoundation";
    case D3DVideoEncoderBackendType::NvencD3D11: return "NvencD3D11";
    case D3DVideoEncoderBackendType::NvencD3D12: return "NvencD3D12";
    case D3DVideoEncoderBackendType::D3D12VideoEncode: return "D3D12VideoEncode";
    default: return "Unknown";
    }
}

inline const char* ToString(VideoCodec codec) noexcept {
    switch (codec) {
    case VideoCodec::H264: return "H264";
    case VideoCodec::HEVC: return "HEVC";
    case VideoCodec::AV1:  return "AV1";
    default: return "Unknown";
    }
}

inline const char* ToString(VideoPixelFormat format) noexcept {
    switch (format) {
    case VideoPixelFormat::NV12: return "NV12";
    case VideoPixelFormat::P010: return "P010";
    case VideoPixelFormat::BGRA8: return "BGRA8";
    case VideoPixelFormat::RGBA8: return "RGBA8";
    case VideoPixelFormat::RGBA16F: return "RGBA16F";
    default: return "Unknown";
    }
}

} // namespace D3DVideoEncoderLib
