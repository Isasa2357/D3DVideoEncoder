#include "test_config.hpp"

#include <D3DVideoEncoder/D3D11VideoEncoder.hpp>

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#include <D3D11Helper/D3D11Framework/D3D11Framework.hpp>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <vector>

using namespace D3DVideoEncoderLib;
using namespace D3D11CoreLib;

namespace {
void fill_bgra_frame(std::vector<uint8_t>& bgra, uint32_t width, uint32_t height, uint32_t frameIndex) {
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            bgra[i + 0] = static_cast<uint8_t>((x * 3 + frameIndex * 17) & 0xff);
            bgra[i + 1] = static_cast<uint8_t>((y * 5 + frameIndex * 7) & 0xff);
            bgra[i + 2] = static_cast<uint8_t>((x + y + frameIndex * 11) & 0xff);
            bgra[i + 3] = 255;
        }
    }
}
}

int main() {
    try {
        const std::filesystem::path shaderDir = std::filesystem::path(D3DVIDEOENCODER_TEST_D3D11_SHADER_DIR);
        if (!std::filesystem::exists(shaderDir)) {
            std::cerr << "D3D11Processing shader directory does not exist: " << shaderDir.string() << "\n";
            return 1;
        }

        D3D11CoreConfig cfg;
        cfg.enableDebugLayer = false;
        cfg.enableInfoQueue = false;
        cfg.allowWarpAdapter = false;
        auto core = D3D11Core::CreateShared(cfg);

        const auto cap = D3D11VideoEncoder::QueryNvencSupport(core.get(), VideoCodec::H264, VideoPixelFormat::NV12);
        if (!cap.supported) {
            std::cout << "SKIP: NVENC D3D11 H.264/NV12 is not available: " << cap.message << "\n";
            return 0;
        }

        constexpr uint32_t width = 320;
        constexpr uint32_t height = 180;
        constexpr uint32_t frames = 30;
        const std::filesystem::path outputPath = std::filesystem::absolute("nvenc_d3d11_h264_smoke.mp4");
        std::error_code ec;
        std::filesystem::remove(outputPath, ec);

        auto src = CreateTexture2D(*core, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT, 0);

        D3D11VideoEncoderDesc desc;
        desc.outputPath = outputPath.wstring();
        desc.width = width;
        desc.height = height;
        desc.frameRateNum = 30;
        desc.frameRateDen = 1;
        desc.backend = D3DVideoEncoderBackendType::NvencD3D11;
        desc.codec = VideoCodec::H264;
        desc.internalFormat = VideoPixelFormat::NV12;
        desc.bitrate = 4'000'000;
        desc.input.core = core.get();
        desc.input.inputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.input.processingShaderDirectory = shaderDir;
        desc.enableDebugLog = true;

        D3D11VideoEncoder encoder(desc);
        std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4);
        for (uint32_t f = 0; f < frames; ++f) {
            fill_bgra_frame(bgra, width, height, f);
            UpdateTexture2D(*core, src, bgra.data(), width, height, DXGI_FORMAT_B8G8R8A8_UNORM, width * 4);
            encoder.write(src.AsTexture2D());
        }
        encoder.close();

        if (!std::filesystem::exists(outputPath) || std::filesystem::file_size(outputPath) <= 1024) {
            std::cerr << "NVENC D3D11 MP4 output was not created or is too small.\n";
            return 1;
        }
        std::cout << "NVENC D3D11 H.264 MP4 smoke wrote " << outputPath.string() << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "NVENC D3D11 H.264 MP4 smoke failed: " << e.what() << "\n";
        return 1;
    }
}
