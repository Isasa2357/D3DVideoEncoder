#include <D3DVideoEncoder/D3DVideoEncoder.hpp>
#include <D3DVideoEncoder/D3DVideoEncoderTypes.hpp>

#include <cstdlib>
#include <iostream>
#include <string>

using namespace D3DVideoEncoderLib;

namespace {
void require_true(bool cond, const char* message) {
    if (!cond) {
        std::cerr << "FAILED: " << message << "\n";
        std::exit(1);
    }
}
}

int main() {
    require_true(ToDxgiFormat(VideoPixelFormat::NV12) == DXGI_FORMAT_NV12, "NV12 maps to DXGI_FORMAT_NV12");
    require_true(ToDxgiFormat(VideoPixelFormat::P010) == DXGI_FORMAT_P010, "P010 maps to DXGI_FORMAT_P010");
    require_true(ToDxgiFormat(VideoPixelFormat::BGRA8) == DXGI_FORMAT_B8G8R8A8_UNORM, "BGRA8 maps to BGRA DXGI format");
    require_true(ToDxgiFormat(VideoPixelFormat::RGBA8) == DXGI_FORMAT_R8G8B8A8_UNORM, "RGBA8 maps to RGBA DXGI format");
    require_true(ToDxgiFormat(VideoPixelFormat::RGBA16F) == DXGI_FORMAT_R16G16B16A16_FLOAT, "RGBA16F maps to float16 RGBA DXGI format");

    require_true(IsSupportedD3D11InputFormat(DXGI_FORMAT_NV12), "D3D11 supports NV12 input");
    require_true(IsSupportedD3D11InputFormat(DXGI_FORMAT_P010), "D3D11 supports P010 input");
    require_true(IsSupportedD3D11InputFormat(DXGI_FORMAT_B8G8R8A8_UNORM), "D3D11 supports BGRA8 input");
    require_true(IsSupportedD3D11InputFormat(DXGI_FORMAT_R8G8B8A8_UNORM), "D3D11 supports RGBA8 input");
    require_true(IsSupportedD3D11InputFormat(DXGI_FORMAT_R16G16B16A16_FLOAT), "D3D11 supports RGBA16F input");
    require_true(IsSupportedD3D12InputFormat(DXGI_FORMAT_R16G16B16A16_FLOAT), "D3D12 supports RGBA16F input");
    require_true(!IsSupportedD3D11InputFormat(DXGI_FORMAT_D32_FLOAT), "D3D11 rejects depth input");

    require_true(IsYuv420EncodeFormat(VideoPixelFormat::NV12), "NV12 is encode format");
    require_true(IsYuv420EncodeFormat(VideoPixelFormat::P010), "P010 is encode format");
    require_true(!IsYuv420EncodeFormat(VideoPixelFormat::RGBA8), "RGBA8 is not encode format");

    require_true(std::string(ToString(D3DVideoInputApi::D3D11)) == "D3D11", "D3D11 ToString");
    require_true(std::string(ToString(D3DVideoInputApi::D3D12)) == "D3D12", "D3D12 ToString");
    require_true(std::string(ToString(D3DVideoEncoderBackendType::MediaFoundation)) == "MediaFoundation", "MediaFoundation ToString");
    require_true(std::string(ToString(VideoCodec::H264)) == "H264", "H264 ToString");
    require_true(std::string(ToString(VideoCodec::HEVC)) == "HEVC", "HEVC ToString");
    require_true(std::string(ToString(VideoPixelFormat::P010)) == "P010", "P010 ToString");
    require_true(std::string(ToString(VideoProcessingFilter::Linear)) == "Linear", "Linear filter ToString");

    using D3D11VariableDurationWrite = void (D3D11VideoEncoder::*)(ID3D11Texture2D*, int64_t, int64_t);
    using D3D12VariableDurationWrite = void (D3D12VideoEncoder::*)(ID3D12Resource*, D3D12_RESOURCE_STATES, int64_t, int64_t);
    using WrapperD3D11VariableDurationWrite = void (D3DVideoEncoder::*)(ID3D11Texture2D*, int64_t, int64_t);
    using WrapperD3D12VariableDurationWrite = void (D3DVideoEncoder::*)(ID3D12Resource*, D3D12_RESOURCE_STATES, int64_t, int64_t);
    D3D11VariableDurationWrite d3d11VariableWrite = &D3D11VideoEncoder::write;
    D3D12VariableDurationWrite d3d12VariableWrite = &D3D12VideoEncoder::write;
    WrapperD3D11VariableDurationWrite wrapperD3D11VariableWrite = &D3DVideoEncoder::write;
    WrapperD3D12VariableDurationWrite wrapperD3D12VariableWrite = &D3DVideoEncoder::write;
    (void)d3d11VariableWrite;
    (void)d3d12VariableWrite;
    (void)wrapperD3D11VariableWrite;
    (void)wrapperD3D12VariableWrite;

    VideoProcessingRect emptyRect;
    require_true(emptyRect.isEmpty(), "default VideoProcessingRect means full source");
    VideoProcessingRect cropRect{ 2, 4, 128, 64 };
    require_true(!cropRect.isEmpty(), "non-empty VideoProcessingRect is treated as crop rect");

    return 0;
}
