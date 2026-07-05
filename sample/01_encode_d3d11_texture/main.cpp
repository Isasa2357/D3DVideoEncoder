#include <D3DVideoEncoder/D3D11VideoEncoder.hpp>

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#include <D3D11Helper/D3D11Framework/D3D11Framework.hpp>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace D3DVideoEncoderLib;
using namespace D3D11CoreLib;

int wmain(int argc, wchar_t** argv) {
    try {
        const std::wstring outputPath = (argc >= 2) ? argv[1] : L"d3d_video_encoder_sample.mp4";
        const std::filesystem::path shaderDir = (argc >= 3) ? argv[2] : L"shaders/D3D11Processing";

        D3D11CoreConfig cfg;
        cfg.allowWarpAdapter = true;
        auto core = D3D11Core::CreateShared(cfg);

        constexpr uint32_t width = 1280;
        constexpr uint32_t height = 720;
        constexpr uint32_t fps = 60;
        constexpr uint32_t frames = 180;

        auto src = CreateTexture2D(
            *core,
            width,
            height,
            DXGI_FORMAT_B8G8R8A8_UNORM,
            D3D11_BIND_SHADER_RESOURCE,
            D3D11_USAGE_DEFAULT,
            0);

        D3D11VideoEncoderDesc desc;
        desc.outputPath = outputPath;
        desc.width = width;
        desc.height = height;
        desc.frameRateNum = fps;
        desc.frameRateDen = 1;
        desc.backend = D3DVideoEncoderBackendType::MediaFoundation;
        desc.codec = VideoCodec::H264;
        desc.internalFormat = VideoPixelFormat::NV12;
        desc.bitrate = 40'000'000;
        desc.input.core = core.get();
        desc.input.inputFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.input.processingShaderDirectory = shaderDir;
        desc.enableDebugLog = true;

        D3D11VideoEncoder encoder(desc);

        std::vector<uint8_t> bgra(width * height * 4);
        for (uint32_t f = 0; f < frames; ++f) {
            for (uint32_t y = 0; y < height; ++y) {
                for (uint32_t x = 0; x < width; ++x) {
                    const size_t i = (static_cast<size_t>(y) * width + x) * 4;
                    bgra[i + 0] = static_cast<uint8_t>((x + f * 3) & 0xff);       // B
                    bgra[i + 1] = static_cast<uint8_t>((y + f * 2) & 0xff);       // G
                    bgra[i + 2] = static_cast<uint8_t>((x + y + f * 5) & 0xff);   // R
                    bgra[i + 3] = 255;
                }
            }

            UpdateTexture2D(*core, src, bgra.data(), width, height, DXGI_FORMAT_B8G8R8A8_UNORM, width * 4);
            encoder.write(src.AsTexture2D());
        }

        encoder.close();
        std::wcout << L"wrote: " << outputPath << L"\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
