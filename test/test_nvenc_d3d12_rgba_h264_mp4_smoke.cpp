#include "test_config.hpp"

#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>

#include <D3D12Helper/D3D12Core/D3D12Core.hpp>
#include <D3D12Helper/D3D12Framework/D3D12Helpers.hpp>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <vector>

using namespace D3DVideoEncoderLib;
using namespace D3D12CoreLib;

namespace {
void fill_rgba_frame(std::vector<uint8_t>& rgba, uint32_t width, uint32_t height, uint32_t frameIndex) {
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            rgba[i + 0] = static_cast<uint8_t>((x * 3 + frameIndex * 17) & 0xff);
            rgba[i + 1] = static_cast<uint8_t>((y * 5 + frameIndex * 7) & 0xff);
            rgba[i + 2] = static_cast<uint8_t>((x + y + frameIndex * 11) & 0xff);
            rgba[i + 3] = 255;
        }
    }
}
}

int main() {
    try {
        const std::filesystem::path shaderDir = std::filesystem::path(D3DVIDEOENCODER_TEST_D3D12_SHADER_DIR);
        if (!std::filesystem::exists(shaderDir)) {
            std::cerr << "D3D12Processing shader directory does not exist: " << shaderDir.string() << "\n";
            return 1;
        }

        D3D12CoreConfig cfg;
        cfg.enableDebugLayer = false;
        cfg.enableInfoQueue = false;
        cfg.enableDred = false;
        cfg.allowWarpAdapter = false;
        auto core = D3D12Core::CreateShared(cfg);

        const auto cap = D3D12VideoEncoder::QueryNvencSupport(core.get(), VideoCodec::H264, VideoPixelFormat::NV12);
        if (!cap.supported) {
            std::cout << "SKIP: NVENC D3D12 H.264/NV12 is not available: " << cap.message << "\n";
            return 0;
        }

        constexpr uint32_t width = 320;
        constexpr uint32_t height = 180;
        constexpr uint32_t frames = 10;
        const std::filesystem::path outputPath = std::filesystem::absolute("nvenc_d3d12_rgba_h264_smoke.mp4");
        std::error_code ec;
        std::filesystem::remove(outputPath, ec);

        D3D12VideoEncoderDesc desc;
        desc.outputPath = outputPath.wstring();
        desc.width = width;
        desc.height = height;
        desc.frameRateNum = 30;
        desc.frameRateDen = 1;
        desc.backend = D3DVideoEncoderBackendType::NvencD3D12;
        desc.codec = VideoCodec::H264;
        desc.internalFormat = VideoPixelFormat::NV12;
        desc.bitrate = 4'000'000;
        desc.input.core = core.get();
        desc.input.inputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.input.processingShaderDirectory = shaderDir;
        desc.enableDebugLog = true;

        D3D12VideoEncoder encoder(desc);
        std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
        for (uint32_t f = 0; f < frames; ++f) {
            fill_rgba_frame(rgba, width, height, f);
            D3D12Resource src = CreateTexture2DFromRGBA(*core, rgba.data(), width, height, D3D12_RESOURCE_STATE_COMMON);
            encoder.write(src.Get(), D3D12_RESOURCE_STATE_COMMON);
        }
        encoder.close();

        if (!std::filesystem::exists(outputPath) || std::filesystem::file_size(outputPath) <= 1024) {
            std::cerr << "NVENC D3D12 MP4 output was not created or is too small.\n";
            return 1;
        }
        std::cout << "NVENC D3D12 RGBA->H.264 MP4 smoke wrote " << outputPath.string() << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "NVENC D3D12 RGBA H.264 MP4 smoke failed: " << e.what() << "\n";
        return 1;
    }
}
