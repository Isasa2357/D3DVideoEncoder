#pragma once

#include <D3DVideoEncoder/D3DVideoEncoderDesc.hpp>
#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

#include <Windows.h>
#include <nvEncodeAPI.h>

#include <cstdint>
#include <fstream>
#include <string>

namespace D3DVideoEncoderLib {

std::string NvencStatusToString(NVENCSTATUS status);
[[noreturn]] void ThrowNvenc(NVENCSTATUS status, const char* expression, const char* file, int line, const std::string& message = {});
void CheckNvenc(NVENCSTATUS status, const char* expression, const char* file, int line, const std::string& message = {});

#define D3DVE_THROW_IF_NVENC_FAILED(expr) \
    ::D3DVideoEncoderLib::CheckNvenc((expr), #expr, __FILE__, __LINE__)

#define D3DVE_THROW_IF_NVENC_FAILED_MSG(expr, msg) \
    ::D3DVideoEncoderLib::CheckNvenc((expr), #expr, __FILE__, __LINE__, (msg))

struct NvencSessionDesc {
    std::wstring outputPath;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frameRateNum = 60;
    uint32_t frameRateDen = 1;
    uint32_t bitrate = 50'000'000;
    uint32_t gopLength = 60;
    uint32_t bFrameCount = 0;
    VideoCodec codec = VideoCodec::H264;
    VideoPixelFormat inputFormat = VideoPixelFormat::NV12;
    VideoRateControlMode rateControl = VideoRateControlMode::VBR;
};

class NvencApi {
public:
    NvencApi() = default;
    ~NvencApi();

    NvencApi(const NvencApi&) = delete;
    NvencApi& operator=(const NvencApi&) = delete;

    void load();
    NV_ENCODE_API_FUNCTION_LIST& functions() noexcept { return functions_; }
    const NV_ENCODE_API_FUNCTION_LIST& functions() const noexcept { return functions_; }

private:
    HMODULE module_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST functions_ = {};
};

class NvencEncoderSession {
public:
    NvencEncoderSession() = default;
    ~NvencEncoderSession();

    NvencEncoderSession(const NvencEncoderSession&) = delete;
    NvencEncoderSession& operator=(const NvencEncoderSession&) = delete;

    void initialize(void* device, NV_ENC_DEVICE_TYPE deviceType, const NvencSessionDesc& desc);
    void encodeDirectXResource(void* resource, int64_t timestamp100ns, int64_t duration100ns);
    void flush();
    void close();

    bool isOpen() const noexcept { return encoder_ != nullptr; }

private:
    GUID codecGuid() const;
    GUID profileGuid() const;
    GUID presetGuid() const;
    NV_ENC_BUFFER_FORMAT bufferFormat() const;
    NV_ENC_PARAMS_RC_MODE rateControlMode() const;
    std::string describe() const;

    void createBitstreamBuffer();
    void destroyBitstreamBuffer() noexcept;
    void writeBitstream(void* bitstreamBuffer);

    NvencApi api_;
    NvencSessionDesc desc_ = {};
    void* encoder_ = nullptr;
    void* bitstreamBuffer_ = nullptr;
    std::ofstream output_;
    bool eosSent_ = false;
};

} // namespace D3DVideoEncoderLib
