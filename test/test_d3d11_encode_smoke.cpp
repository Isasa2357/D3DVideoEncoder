#include "test_config.hpp"

#include <D3DVideoEncoder/D3D11VideoEncoder.hpp>

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#include <D3D11Helper/D3D11Framework/D3D11Framework.hpp>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <objbase.h>

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace D3DVideoEncoderLib;
using namespace D3D11CoreLib;

namespace {

void require_true(bool cond, const std::string& message) {
    if (!cond) {
        std::cerr << "FAILED: " << message << "\n";
        std::exit(1);
    }
}

void throw_if_failed(HRESULT hr, const char* expr) {
    if (FAILED(hr)) {
        std::ostringstream oss;
        oss << expr << " failed: HRESULT=0x" << std::hex << static_cast<unsigned long>(hr);
        throw std::runtime_error(oss.str());
    }
}

class ComApartment {
public:
    ComApartment() {
        hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr_) && hr_ != RPC_E_CHANGED_MODE) {
            throw_if_failed(hr_, "CoInitializeEx");
        }
    }
    ~ComApartment() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }
private:
    HRESULT hr_ = S_OK;
};

class MfRuntimeForReadback {
public:
    MfRuntimeForReadback() {
        throw_if_failed(MFStartup(MF_VERSION, MFSTARTUP_FULL), "MFStartup");
    }
    ~MfRuntimeForReadback() {
        MFShutdown();
    }
};

struct Mp4VideoInfo {
    UINT32 width = 0;
    UINT32 height = 0;
    UINT32 frameRateNum = 0;
    UINT32 frameRateDen = 0;
    GUID subtype = GUID_NULL;
    uint64_t sampleCount = 0;
    LONGLONG firstTimestamp = 0;
    LONGLONG lastTimestamp = 0;
};

Mp4VideoInfo read_video_info_with_source_reader(const std::filesystem::path& path) {
    ComApartment com;
    MfRuntimeForReadback mf;

    ComPtr<IMFSourceReader> reader;
    throw_if_failed(MFCreateSourceReaderFromURL(path.wstring().c_str(), nullptr, &reader),
                    "MFCreateSourceReaderFromURL");

    throw_if_failed(reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE),
                    "IMFSourceReader::SetStreamSelection(all,false)");
    throw_if_failed(reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE),
                    "IMFSourceReader::SetStreamSelection(video,true)");

    ComPtr<IMFMediaType> nativeType;
    throw_if_failed(reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &nativeType),
                    "IMFSourceReader::GetNativeMediaType");

    Mp4VideoInfo info;
    throw_if_failed(MFGetAttributeSize(nativeType.Get(), MF_MT_FRAME_SIZE, &info.width, &info.height),
                    "MFGetAttributeSize(MF_MT_FRAME_SIZE)");

    // Some files/transforms may omit this attribute. Width/height and readable samples are mandatory.
    MFGetAttributeRatio(nativeType.Get(), MF_MT_FRAME_RATE, &info.frameRateNum, &info.frameRateDen);
    nativeType->GetGUID(MF_MT_SUBTYPE, &info.subtype);

    for (uint32_t guard = 0; guard < 10000; ++guard) {
        DWORD actualStreamIndex = 0;
        DWORD streamFlags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;

        throw_if_failed(reader->ReadSample(
                            MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                            0,
                            &actualStreamIndex,
                            &streamFlags,
                            &timestamp,
                            &sample),
                        "IMFSourceReader::ReadSample");

        if ((streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            break;
        }

        if (sample) {
            if (info.sampleCount == 0) {
                info.firstTimestamp = timestamp;
            }
            info.lastTimestamp = timestamp;
            ++info.sampleCount;
        }
    }

    return info;
}

void fill_bgra_frame(std::vector<uint8_t>& bgra, uint32_t width, uint32_t height, uint32_t frameIndex) {
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t i = (static_cast<size_t>(y) * width + x) * 4;
            bgra[i + 0] = static_cast<uint8_t>((x * 3 + frameIndex * 7) & 0xff);       // B
            bgra[i + 1] = static_cast<uint8_t>((y * 5 + frameIndex * 11) & 0xff);      // G
            bgra[i + 2] = static_cast<uint8_t>((x + y + frameIndex * 13) & 0xff);      // R
            bgra[i + 3] = 255;
        }
    }
}

} // namespace

int main() {
    try {
        const std::filesystem::path shaderDir = std::filesystem::path(D3DVIDEOENCODER_TEST_D3D11_SHADER_DIR);
        require_true(std::filesystem::exists(shaderDir), "D3D11Processing shader directory does not exist: " + shaderDir.string());

        const std::filesystem::path outputPath = std::filesystem::absolute("d3d11_encode_smoke.mp4");
        std::error_code ec;
        std::filesystem::remove(outputPath, ec);

        D3D11CoreConfig cfg;
        cfg.enableDebugLayer = false;
        cfg.enableInfoQueue = false;
        cfg.allowWarpAdapter = true;
        auto core = D3D11Core::CreateShared(cfg);

        constexpr uint32_t width = 320;
        constexpr uint32_t height = 180;
        constexpr uint32_t fps = 30;
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
        desc.frameRateNum = fps;
        desc.frameRateDen = 1;
        desc.backend = D3DVideoEncoderBackendType::MediaFoundation;
        desc.codec = VideoCodec::H264;
        desc.internalFormat = VideoPixelFormat::NV12;
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

        require_true(std::filesystem::exists(outputPath), "encoded MP4 was not created");
        require_true(std::filesystem::file_size(outputPath) > 1024, "encoded MP4 is unexpectedly small");

        const Mp4VideoInfo info = read_video_info_with_source_reader(outputPath);
        require_true(info.width == width, "readback width mismatch");
        require_true(info.height == height, "readback height mismatch");
        require_true(info.sampleCount >= frames, "readback sample count is smaller than written frame count");
        require_true(info.lastTimestamp >= info.firstTimestamp, "readback timestamps are not monotonic enough to validate");

        std::cout << "D3D11EncodeSmoke wrote and read back " << info.sampleCount
                  << " video samples from " << outputPath.string() << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "D3D11 encode smoke test failed: " << e.what() << "\n";
        return 1;
    }
}
