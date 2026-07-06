#pragma once

#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>

namespace D3DVideoEncoderLib {

D3D12VideoEncodeFormatCapability QueryD3D12VideoEncodeDeviceSupport(
    D3D12CoreLib::D3D12Core* core,
    VideoCodec codec,
    VideoPixelFormat inputFormat,
    uint32_t width,
    uint32_t height);

D3D12VideoEncodeCapabilities QueryD3D12VideoEncodeDeviceCapabilities(
    D3D12CoreLib::D3D12Core* core,
    uint32_t width,
    uint32_t height);

} // namespace D3DVideoEncoderLib
