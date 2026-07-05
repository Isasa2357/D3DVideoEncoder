#include <D3DVideoEncoder/D3DVideoEncoder.hpp>
#include <D3DVideoEncoder/D3D11VideoEncoder.hpp>

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

D3D11VideoEncoderDesc valid_until_core_required() {
    D3D11VideoEncoderDesc desc;
    desc.outputPath = L"validation_test.mp4";
    desc.width = 1920;
    desc.height = 1080;
    desc.frameRateNum = 60;
    desc.frameRateDen = 1;
    desc.backend = D3DVideoEncoderBackendType::MediaFoundation;
    desc.codec = VideoCodec::H264;
    desc.internalFormat = VideoPixelFormat::NV12;
    desc.bitrate = 5000000;
    desc.asyncMode = false;
    desc.enableDebugLog = false;
    return desc;
}
}

int main() {
    expect_encoder_error("empty output path", [] {
        auto desc = valid_until_core_required();
        desc.outputPath.clear();
        D3D11VideoEncoder encoder(desc);
    }, "outputPath");

    expect_encoder_error("zero width", [] {
        auto desc = valid_until_core_required();
        desc.width = 0;
        D3D11VideoEncoder encoder(desc);
    }, "width/height");

    expect_encoder_error("odd width", [] {
        auto desc = valid_until_core_required();
        desc.width = 1919;
        D3D11VideoEncoder encoder(desc);
    }, "even");

    expect_encoder_error("zero fps numerator", [] {
        auto desc = valid_until_core_required();
        desc.frameRateNum = 0;
        D3D11VideoEncoder encoder(desc);
    }, "frameRateNum");

    expect_encoder_error("zero bitrate", [] {
        auto desc = valid_until_core_required();
        desc.bitrate = 0;
        D3D11VideoEncoder encoder(desc);
    }, "bitrate");

    expect_encoder_error("h264 p010 rejected", [] {
        auto desc = valid_until_core_required();
        desc.internalFormat = VideoPixelFormat::P010;
        D3D11VideoEncoder encoder(desc);
    }, "H.264");

    expect_encoder_error("zero queue depth", [] {
        auto desc = valid_until_core_required();
        desc.queueDepth = 0;
        D3D11VideoEncoder encoder(desc);
    }, "queueDepth");

    expect_encoder_error("missing d3d11 core", [] {
        auto desc = valid_until_core_required();
        D3D11VideoEncoder encoder(desc);
    }, "input.core");

    expect_encoder_error("legacy wrapper missing d3d11 core", [] {
        D3DVideoEncoderDesc desc;
        desc.outputPath = L"validation_test.mp4";
        desc.width = 1920;
        desc.height = 1080;
        desc.frameRateNum = 60;
        desc.frameRateDen = 1;
        desc.inputApi = D3DVideoInputApi::D3D11;
        desc.backend = D3DVideoEncoderBackendType::MediaFoundation;
        desc.codec = VideoCodec::H264;
        desc.internalFormat = VideoPixelFormat::NV12;
        desc.bitrate = 5000000;
        desc.enableDebugLog = false;
        D3DVideoEncoder encoder(desc);
    }, "input.core");

    require_true(true, "validation tests completed");
    return 0;
}
