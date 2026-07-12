#pragma once

#include <D3DVideoEncoder/D3DVideoEncoderDesc.hpp>
#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

#include <Windows.h>
#include <nvEncodeAPI.h>
#include "backend/mux/NvencOutputMuxer.hpp"

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <wrl/client.h>

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

NvencFormatCapability QueryNvencDeviceSupport(void* device, NV_ENC_DEVICE_TYPE deviceType, VideoCodec codec, VideoPixelFormat inputFormat);

enum class NvencDirectXDeviceKind {
    D3D11,
    D3D12
};

class NvencD3D12OutputStrategy;

class NvencEncoderSession {
public:
    NvencEncoderSession();
    ~NvencEncoderSession();

    NvencEncoderSession(const NvencEncoderSession&) = delete;
    NvencEncoderSession& operator=(const NvencEncoderSession&) = delete;

    void initialize(
        void* device,
        NV_ENC_DEVICE_TYPE deviceType,
        NvencDirectXDeviceKind deviceKind,
        const NvencSessionDesc& desc);
    void encodeDirectXResource(void* resource, int64_t timestamp100ns, int64_t duration100ns);
    void flush();
    void close();

    bool isOpen() const noexcept { return encoder_ != nullptr; }

private:
    struct RegisteredInputResource {
        // Raw ID3D11Texture2D*/ID3D12Resource* used as the lookup key.
        void* resourceKey = nullptr;

        // Handle returned by nvEncRegisterResource.
        void* registeredResource = nullptr;

        // Keeps the original DirectX resource alive while it is registered with NVENC.
        Microsoft::WRL::ComPtr<IUnknown> keepAlive;
    };

    GUID codecGuid() const;
    GUID profileGuid() const;
    GUID presetGuid() const;
    NV_ENC_BUFFER_FORMAT bufferFormat() const;
    NV_ENC_PARAMS_RC_MODE rateControlMode() const;
    std::string describe() const;

    void createBitstreamBuffer();
    void destroyBitstreamBuffer() noexcept;
    void registerAsyncEvent();
    void unregisterAsyncEvent() noexcept;
    bool waitForAsyncCompletion(const char* operation, uint32_t frameIndex);
    void writeBitstream(void* bitstreamBuffer);
    void encodeD3D12Resource(void* resource, int64_t timestamp100ns, int64_t duration100ns);
    void flushD3D12();
    void trace(const std::string& message) const;
    RegisteredInputResource& getOrRegisterResource(void* resource);
    void unregisterAllResources() noexcept;

    NvencApi api_;
    NvencSessionDesc desc_ = {};
    void* encoder_ = nullptr;
    void* bitstreamBuffer_ = nullptr;
    NvencDirectXDeviceKind deviceKind_ = NvencDirectXDeviceKind::D3D11;
    std::unique_ptr<NvencD3D12OutputStrategy> d3d12Output_;
    NvencOutputMuxer muxer_;
    std::vector<RegisteredInputResource> registeredResources_;
    bool eosSent_ = false;
    bool traceEnabled_ = false;
    bool internalAsync_ = false;
    HANDLE completionEvent_ = nullptr;
    bool completionEventRegistered_ = false;
    uint64_t sentFrameCount_ = 0;
    uint64_t receivedPacketCount_ = 0;
    int64_t currentTimestamp100ns_ = 0;
    int64_t currentDuration100ns_ = 0;
};

} // namespace D3DVideoEncoderLib
