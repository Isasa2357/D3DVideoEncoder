#include <D3DVideoEncoder/D3D11VideoEncoder.hpp>

#include "test_config.hpp"

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>

#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace D3DVideoEncoderLib;

namespace {

struct Options {
    std::filesystem::path input;
    std::filesystem::path output = "nvenc_d3d11_direct_nv12_30f.h264";
    std::string mode = "direct";
    uint32_t frames = 30;
    bool async = false;
    bool streamInput = false;
};

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string out(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), required, nullptr, nullptr);
    return out;
}

Options parse_args(int argc, wchar_t** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto require_value = [&](const wchar_t* name) -> const wchar_t* {
            if (i + 1 >= argc) {
                throw std::runtime_error("Missing value for " + narrow(name));
            }
            return argv[++i];
        };

        if (arg == L"--input") {
            options.input = require_value(L"--input");
        } else if (arg == L"--output") {
            options.output = require_value(L"--output");
        } else if (arg == L"--mode") {
            options.mode = narrow(require_value(L"--mode"));
        } else if (arg == L"--frames") {
            options.frames = static_cast<uint32_t>(std::wcstoul(require_value(L"--frames"), nullptr, 10));
        } else if (arg == L"--async") {
            options.async = true;
        } else if (arg == L"--stream-input") {
            options.streamInput = true;
        } else {
            throw std::runtime_error("Unknown argument: " + narrow(arg));
        }
    }
    if (options.frames == 0) {
        throw std::runtime_error("--frames must be non-zero.");
    }
    if (options.mode != "direct" && options.mode != "bgra") {
        throw std::runtime_error("--mode must be direct or bgra.");
    }
    return options;
}

DXGI_FORMAT input_format(const Options& options) {
    return options.mode == "bgra" ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_NV12;
}

size_t frame_size(DXGI_FORMAT format, uint32_t width, uint32_t height) {
    if (format == DXGI_FORMAT_B8G8R8A8_UNORM) return static_cast<size_t>(width) * height * 4;
    return static_cast<size_t>(width) * height * 3 / 2;
}

std::vector<uint8_t> load_or_generate_input(const Options& options, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    const size_t frameSize = frame_size(format, width, height);
    std::vector<uint8_t> bytes(frameSize * options.frames);
    if (!options.input.empty()) {
        std::ifstream file(options.input, std::ios::binary);
        if (!file) {
            throw std::runtime_error("Failed to open input raw file: " + options.input.string());
        }
        file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (file.gcount() != static_cast<std::streamsize>(bytes.size())) {
            throw std::runtime_error("Input raw file is smaller than requested frame count.");
        }
        return bytes;
    }

    for (uint32_t f = 0; f < options.frames; ++f) {
        uint8_t* frame = bytes.data() + frameSize * f;
        if (format == DXGI_FORMAT_NV12) {
            for (uint32_t y = 0; y < height; ++y) {
                uint8_t* row = frame + static_cast<size_t>(y) * width;
                for (uint32_t x = 0; x < width; ++x) {
                    row[x] = static_cast<uint8_t>(48 + ((x + y + f * 7u) & 0x3f));
                }
            }
            std::fill(frame + static_cast<size_t>(width) * height, frame + frameSize, 128);
        } else {
            for (uint32_t y = 0; y < height; ++y) {
                uint8_t* row = frame + static_cast<size_t>(y) * width * 4;
                for (uint32_t x = 0; x < width; ++x) {
                    const size_t p = static_cast<size_t>(x) * 4;
                    row[p + 0] = static_cast<uint8_t>((x + f * 5u) & 0xffu);
                    row[p + 1] = static_cast<uint8_t>((y + f * 3u) & 0xffu);
                    row[p + 2] = static_cast<uint8_t>((x + y + f * 11u) & 0xffu);
                    row[p + 3] = 255;
                }
            }
        }
    }
    return bytes;
}

Microsoft::WRL::ComPtr<ID3D11Texture2D> create_input_texture(D3D11CoreLib::D3D11Core& core, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = core.GetDevice()->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(hr)) {
        throw std::runtime_error("CreateTexture2D(input) failed.");
    }
    return texture;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        constexpr uint32_t width = 640;
        constexpr uint32_t height = 360;
        constexpr uint32_t fps = 30;
        const Options options = parse_args(argc, argv);
        const DXGI_FORMAT format = input_format(options);
        const auto raw = options.streamInput ? std::vector<uint8_t>{} : load_or_generate_input(options, width, height, format);
        const size_t frameSize = frame_size(format, width, height);
        std::ifstream streamedInput;
        std::vector<uint8_t> streamedFrame;
        if (options.streamInput) {
            if (options.input.empty()) {
                throw std::runtime_error("--stream-input requires --input.");
            }
            streamedInput.open(options.input, std::ios::binary);
            if (!streamedInput) {
                throw std::runtime_error("Failed to open streaming raw input.");
            }
            streamedFrame.resize(frameSize);
        }

        if (options.output.has_parent_path()) {
            std::filesystem::create_directories(options.output.parent_path());
        }
        std::error_code ec;
        std::filesystem::remove(options.output, ec);

        D3D11CoreLib::D3D11CoreConfig cfg;
        cfg.enableDebugLayer = false;
        cfg.enableInfoQueue = false;
        cfg.allowWarpAdapter = false;
        auto core = D3D11CoreLib::D3D11Core::CreateShared(cfg);

        const auto cap = D3D11VideoEncoder::QueryNvencSupport(core.get(), VideoCodec::H264, VideoPixelFormat::NV12);
        if (!cap.supported) {
            std::cout << "SKIP: NVENC D3D11 H.264/NV12 unavailable: " << cap.message << "\n";
            return 0;
        }

        D3D11VideoEncoderDesc desc;
        desc.outputPath = options.output.wstring();
        desc.width = width;
        desc.height = height;
        desc.frameRateNum = fps;
        desc.frameRateDen = 1;
        desc.backend = D3DVideoEncoderBackendType::NvencD3D11;
        desc.codec = VideoCodec::H264;
        desc.internalFormat = VideoPixelFormat::NV12;
        desc.bitrate = 4'000'000;
        desc.gopLength = 15;
        desc.asyncMode = options.async;
        desc.input.core = core.get();
        desc.input.inputFormat = format;
        desc.input.allowFormatConversion = format != DXGI_FORMAT_NV12;
        desc.input.processingShaderDirectory = D3DVIDEOENCODER_TEST_D3D11_SHADER_DIR;
        desc.enableDebugLog = true;

        auto texture = create_input_texture(*core, width, height, format);
        D3D11VideoEncoder encoder(desc);
        const int64_t duration100ns = 10'000'000 / fps;
        const UINT rowPitch = format == DXGI_FORMAT_NV12 ? width : width * 4;
        for (uint32_t f = 0; f < options.frames; ++f) {
            const uint8_t* frame = nullptr;
            if (options.streamInput) {
                streamedInput.read(reinterpret_cast<char*>(streamedFrame.data()), static_cast<std::streamsize>(streamedFrame.size()));
                if (streamedInput.gcount() != static_cast<std::streamsize>(streamedFrame.size())) {
                    throw std::runtime_error("Streaming raw input is smaller than requested frame count.");
                }
                frame = streamedFrame.data();
            } else {
                frame = raw.data() + frameSize * f;
            }
            core->GetImmediateContext()->UpdateSubresource(texture.Get(), 0, nullptr, frame, rowPitch, 0);
            encoder.write(texture.Get(), static_cast<int64_t>(f) * duration100ns, duration100ns);
            if (f == 0 || f + 1 == options.frames || ((f + 1) % 1000) == 0) {
                std::cout << "written frame " << f << "\n";
            }
        }
        encoder.close();

        const auto size = std::filesystem::exists(options.output) ? std::filesystem::file_size(options.output) : 0;
        std::cout << "NVENC D3D11 " << options.mode
                  << " async=" << (options.async ? "true" : "false")
                  << " wrote frames=" << encoder.writtenFrameCount()
                  << " output=" << options.output.string()
                  << " size=" << size << "\n";
        return (encoder.writtenFrameCount() == options.frames && size > 0) ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "NVENC D3D11 direct NV12 acceptance failed: " << e.what() << "\n";
        return 1;
    }
}
