#include "test_config.hpp"

#include <D3DVideoEncoder/D3D11VideoEncoder.hpp>

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#include <D3D11Helper/D3D11Framework/D3D11Framework.hpp>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace D3DVideoEncoderLib;
using namespace D3D11CoreLib;

namespace {
void fill_bgra_frame(std::vector<uint8_t>& bgra, uint32_t width, uint32_t height, uint32_t frameIndex) {
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            bgra[i + 0] = static_cast<uint8_t>((x * 2 + frameIndex * 5) & 0xff);
            bgra[i + 1] = static_cast<uint8_t>((y * 4 + frameIndex * 3) & 0xff);
            bgra[i + 2] = static_cast<uint8_t>((x + y + frameIndex * 9) & 0xff);
            bgra[i + 3] = 255;
        }
    }
}
}

int main() {
    try {
        const auto caps = D3D11VideoEncoder::QueryMediaFoundationSupport(VideoCodec::HEVC, VideoPixelFormat::P010);
        if (!caps.supported) {
            std::cout << "SKIP: Media Foundation HEVC/P010 encoder is not available on this machine.\n";
            return 0;
        }

        const std::filesystem::path shaderDir = std::filesystem::path(D3DVIDEOENCODER_TEST_D3D11_SHADER_DIR);
        if (!std::filesystem::exists(shaderDir)) {
            std::cerr << "D3D11Processing shader directory does not exist: " << shaderDir.string() << "\n";
            return 1;
        }

        const std::filesystem::path outputPath = std::filesystem::absolute("d3d11_hevc_p010_smoke.mp4");
        std::error_code ec;
        std::filesystem::remove(outputPath, ec);

        D3D11CoreConfig cfg;
        cfg.enableDebugLayer = false;
        cfg.enableInfoQueue = false;
        cfg.allowWarpAdapter = true;
        auto core = D3D11Core::CreateShared(cfg);

        constexpr uint32_t width = 320;
        constexpr uint32_t height = 180;
        constexpr uint32_t frames = 30;

        auto src = CreateTexture2D(
            *core,
            width,
            height,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            D3D11_BIND_SHADER_RESOURCE,
            D3D11_USAGE_DEFAULT,
            0);

        D3D11VideoEncoderDesc desc;
        desc.outputPath = outputPath.wstring();
        desc.width = width;
        desc.height = height;
        desc.frameRateNum = 30;
        desc.frameRateDen = 1;
        desc.backend = D3DVideoEncoderBackendType::MediaFoundation;
        desc.codec = VideoCodec::HEVC;
        desc.internalFormat = VideoPixelFormat::P010;
        desc.bitrate = 4'000'000;
        desc.input.core = core.get();
        desc.input.inputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.input.processingShaderDirectory = shaderDir;
        desc.enableHardwareTransform = true;
        desc.asyncMode = false;
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
            std::cerr << "HEVC/P010 MP4 output was not created or is too small.\n";
            return 1;
        }
        std::cout << "D3D11 HEVC/P010 smoke wrote " << outputPath.string() << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "D3D11 HEVC/P010 smoke failed: " << e.what() << "\n";
        return 1;
    }
}
