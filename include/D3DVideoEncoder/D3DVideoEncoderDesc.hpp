#pragma once

#include "D3DVideoEncoderTypes.hpp"

#include <filesystem>
#include <string>

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

namespace D3DVideoEncoderLib {

struct VideoEncoderCommonDesc {
    std::wstring outputPath;

    uint32_t width = 0;
    uint32_t height = 0;

    uint32_t frameRateNum = 60;
    uint32_t frameRateDen = 1;

    D3DVideoEncoderBackendType backend = D3DVideoEncoderBackendType::MediaFoundation;
    VideoCodec codec = VideoCodec::H264;

    // NV12 for 8-bit H.264/HEVC. P010 is intended for HEVC Main10/high-quality paths.
    VideoPixelFormat internalFormat = VideoPixelFormat::NV12;

    uint32_t bitrate = 50'000'000;
    VideoRateControlMode rateControl = VideoRateControlMode::VBR;

    uint32_t gopLength = 60;
    uint32_t bFrameCount = 0;

    VideoColorRange colorRange = VideoColorRange::Limited;
    VideoColorMatrix colorMatrix = VideoColorMatrix::BT709;

    bool enableHardwareTransform = true;
    bool useOnlyHardwareTransform = false;

    bool asyncMode = false;
    uint32_t queueDepth = 4;
    EncoderQueueFullPolicy queueFullPolicy = EncoderQueueFullPolicy::Block;

    bool enableDebugLog = true;
};

struct D3D11VideoInputDesc {
    // Preferred entry point: let D3D11Helper own device/context/processing.
    D3D11CoreLib::D3D11Core* core = nullptr;

    DXGI_FORMAT inputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    bool allowFormatConversion = true;

    // Required when inputFormat is not already the internal encode format.
    std::filesystem::path processingShaderDirectory = L"shaders/D3D11Processing";
};

struct D3D12VideoInputDesc {
    // D3D12 input is intentionally separated from the D3D11/Media Foundation path.
    // It is prepared for future NvencD3D12 / D3D12 Video Encode backends.
    D3D12CoreLib::D3D12Core* core = nullptr;

    DXGI_FORMAT inputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    bool allowFormatConversion = true;

    std::filesystem::path processingShaderDirectory = L"shaders/D3D12Processing";

    // Conservative defaults for future D3D12Processing/NVENC/D3D12 Video Encode implementations.
    uint32_t cbvSrvUavDescriptorCount = 256;
    uint32_t samplerDescriptorCount = 16;
};

struct D3D11VideoEncoderDesc : VideoEncoderCommonDesc {
    D3D11VideoInputDesc input;
};

struct D3D12VideoEncoderDesc : VideoEncoderCommonDesc {
    D3D12VideoInputDesc input;
};

// Backward-compatible transition descriptor. Prefer D3D11VideoEncoderDesc or
// D3D12VideoEncoderDesc in new code.
struct D3DVideoEncoderDesc : VideoEncoderCommonDesc {
    D3DVideoInputApi inputApi = D3DVideoInputApi::D3D11;
    D3D11VideoInputDesc d3d11;
    D3D12VideoInputDesc d3d12;
};

D3DVideoEncoderDesc ToLegacyDesc(const D3D11VideoEncoderDesc& desc);
D3DVideoEncoderDesc ToLegacyDesc(const D3D12VideoEncoderDesc& desc);
D3D11VideoEncoderDesc ToD3D11Desc(const D3DVideoEncoderDesc& desc);
D3D12VideoEncoderDesc ToD3D12Desc(const D3DVideoEncoderDesc& desc);

} // namespace D3DVideoEncoderLib
