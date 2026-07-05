#include <D3DVideoEncoder/D3DVideoEncoder.hpp>
#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>

#include <cstdlib>
#include <exception>
#include <functional>
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

void expect_encoder_error(const char* name, const std::function<void()>& fn, const std::string& expectedFragment) {
    try {
        fn();
    } catch (const D3DVideoEncoderError& e) {
        const std::string message = e.what();
        if (!expectedFragment.empty() && message.find(expectedFragment) == std::string::npos) {
            std::cerr << "FAILED: " << name << " threw unexpected message: " << message << "\n";
            std::exit(1);
        }
        return;
    } catch (const std::exception& e) {
        std::cerr << "FAILED: " << name << " threw non-encoder exception: " << e.what() << "\n";
        std::exit(1);
    }

    std::cerr << "FAILED: " << name << " did not throw\n";
    std::exit(1);
}

D3D12VideoEncoderDesc valid_d3d12_desc() {
    D3D12VideoEncoderDesc desc;
    desc.outputPath = L"d3d12_future.mp4";
    desc.width = 1920;
    desc.height = 1080;
    desc.frameRateNum = 60;
    desc.frameRateDen = 1;
    desc.backend = D3DVideoEncoderBackendType::MediaFoundation;
    desc.codec = VideoCodec::H264;
    desc.internalFormat = VideoPixelFormat::NV12;
    desc.bitrate = 5000000;
    desc.enableDebugLog = false;
    // Constructor validation only checks non-null. The backend is rejected before any D3D12Core access.
    desc.input.core = reinterpret_cast<D3D12CoreLib::D3D12Core*>(0x1);
    desc.input.inputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    return desc;
}
}

int main() {
    expect_encoder_error("d3d12 mf postponed", [] {
        auto desc = valid_d3d12_desc();
        desc.backend = D3DVideoEncoderBackendType::MediaFoundation;
        D3D12VideoEncoder encoder(desc);
    }, "postponed");

#ifndef D3DVIDEOENCODER_TEST_HAS_NVENC
    expect_encoder_error("d3d12 nvenc planned", [] {
        auto desc = valid_d3d12_desc();
        desc.backend = D3DVideoEncoderBackendType::NvencD3D12;
        D3D12VideoEncoder encoder(desc);
    }, "NvencD3D12");
#endif

#ifndef D3DVIDEOENCODER_TEST_HAS_D3D12_VIDEO_ENCODE
    expect_encoder_error("d3d12 native planned", [] {
        auto desc = valid_d3d12_desc();
        desc.backend = D3DVideoEncoderBackendType::D3D12VideoEncode;
        D3D12VideoEncoder encoder(desc);
    }, "D3D12 Video Encode");
#endif

    expect_encoder_error("legacy wrapper d3d12 mf postponed", [] {
        D3DVideoEncoderDesc desc;
        desc.outputPath = L"d3d12_future.mp4";
        desc.width = 1920;
        desc.height = 1080;
        desc.frameRateNum = 60;
        desc.frameRateDen = 1;
        desc.inputApi = D3DVideoInputApi::D3D12;
        desc.backend = D3DVideoEncoderBackendType::MediaFoundation;
        desc.codec = VideoCodec::H264;
        desc.internalFormat = VideoPixelFormat::NV12;
        desc.bitrate = 5000000;
        desc.enableDebugLog = false;
        desc.d3d12.core = reinterpret_cast<D3D12CoreLib::D3D12Core*>(0x1);
        desc.d3d12.inputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        D3DVideoEncoder encoder(desc);
    }, "postponed");

    expect_encoder_error("d3d12 missing core", [] {
        auto desc = valid_d3d12_desc();
        desc.input.core = nullptr;
        D3D12VideoEncoder encoder(desc);
    }, "input.core");

    require_true(true, "D3D12 unsupported backend tests completed");
    return 0;
}
