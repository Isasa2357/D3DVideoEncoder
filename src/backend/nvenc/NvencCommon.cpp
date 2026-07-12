#include "backend/nvenc/NvencCommon.hpp"
#include "backend/nvenc/NvencD3D12OutputStrategy.hpp"
#include "backend/nvenc/NvencLifecyclePolicy.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <vector>

namespace D3DVideoEncoderLib {

std::string NvencStatusToString(NVENCSTATUS status) {
    switch (status) {
    case NV_ENC_SUCCESS: return "NV_ENC_SUCCESS";
    case NV_ENC_ERR_NO_ENCODE_DEVICE: return "NV_ENC_ERR_NO_ENCODE_DEVICE";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE: return "NV_ENC_ERR_UNSUPPORTED_DEVICE";
    case NV_ENC_ERR_INVALID_ENCODERDEVICE: return "NV_ENC_ERR_INVALID_ENCODERDEVICE";
    case NV_ENC_ERR_INVALID_DEVICE: return "NV_ENC_ERR_INVALID_DEVICE";
    case NV_ENC_ERR_DEVICE_NOT_EXIST: return "NV_ENC_ERR_DEVICE_NOT_EXIST";
    case NV_ENC_ERR_INVALID_PTR: return "NV_ENC_ERR_INVALID_PTR";
    case NV_ENC_ERR_INVALID_EVENT: return "NV_ENC_ERR_INVALID_EVENT";
    case NV_ENC_ERR_INVALID_PARAM: return "NV_ENC_ERR_INVALID_PARAM";
    case NV_ENC_ERR_INVALID_CALL: return "NV_ENC_ERR_INVALID_CALL";
    case NV_ENC_ERR_OUT_OF_MEMORY: return "NV_ENC_ERR_OUT_OF_MEMORY";
    case NV_ENC_ERR_ENCODER_NOT_INITIALIZED: return "NV_ENC_ERR_ENCODER_NOT_INITIALIZED";
    case NV_ENC_ERR_UNSUPPORTED_PARAM: return "NV_ENC_ERR_UNSUPPORTED_PARAM";
    case NV_ENC_ERR_LOCK_BUSY: return "NV_ENC_ERR_LOCK_BUSY";
    case NV_ENC_ERR_NOT_ENOUGH_BUFFER: return "NV_ENC_ERR_NOT_ENOUGH_BUFFER";
    case NV_ENC_ERR_INVALID_VERSION: return "NV_ENC_ERR_INVALID_VERSION";
    case NV_ENC_ERR_MAP_FAILED: return "NV_ENC_ERR_MAP_FAILED";
    case NV_ENC_ERR_NEED_MORE_INPUT: return "NV_ENC_ERR_NEED_MORE_INPUT";
    case NV_ENC_ERR_ENCODER_BUSY: return "NV_ENC_ERR_ENCODER_BUSY";
    case NV_ENC_ERR_EVENT_NOT_REGISTERD: return "NV_ENC_ERR_EVENT_NOT_REGISTERD";
    case NV_ENC_ERR_GENERIC: return "NV_ENC_ERR_GENERIC";
    case NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY: return "NV_ENC_ERR_INCOMPATIBLE_CLIENT_KEY";
    case NV_ENC_ERR_UNIMPLEMENTED: return "NV_ENC_ERR_UNIMPLEMENTED";
    case NV_ENC_ERR_RESOURCE_REGISTER_FAILED: return "NV_ENC_ERR_RESOURCE_REGISTER_FAILED";
    case NV_ENC_ERR_RESOURCE_NOT_REGISTERED: return "NV_ENC_ERR_RESOURCE_NOT_REGISTERED";
    case NV_ENC_ERR_RESOURCE_NOT_MAPPED: return "NV_ENC_ERR_RESOURCE_NOT_MAPPED";
    default: {
        std::ostringstream oss;
        oss << "NVENCSTATUS(" << static_cast<int>(status) << ")";
        return oss.str();
    }
    }
}

[[noreturn]] void ThrowNvenc(NVENCSTATUS status, const char* expression, const char* file, int line, const std::string& message) {
    std::ostringstream oss;
    if (!message.empty()) {
        oss << message << ": ";
    }
    oss << expression << " failed at " << file << ":" << line << " - " << NvencStatusToString(status);
    throw D3DVideoEncoderError(oss.str());
}

void CheckNvenc(NVENCSTATUS status, const char* expression, const char* file, int line, const std::string& message) {
    if (status != NV_ENC_SUCCESS) {
        ThrowNvenc(status, expression, file, line, message);
    }
}

NvencApi::~NvencApi() {
    if (module_) {
        FreeLibrary(module_);
        module_ = nullptr;
    }
}

void NvencApi::load() {
    if (module_) return;

#if defined(_WIN64)
    module_ = LoadLibraryA("nvEncodeAPI64.dll");
#else
    module_ = LoadLibraryA("nvEncodeAPI.dll");
#endif
    if (!module_) {
        throw D3DVideoEncoderError("NVENC runtime DLL was not found. Install/update the NVIDIA display driver. Expected nvEncodeAPI64.dll on x64 Windows.");
    }

    using CreateInstanceFunc = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
    auto* createInstance = reinterpret_cast<CreateInstanceFunc>(GetProcAddress(module_, "NvEncodeAPICreateInstance"));
    if (!createInstance) {
        throw D3DVideoEncoderError("NvEncodeAPICreateInstance was not found in nvEncodeAPI runtime DLL.");
    }

    functions_ = {};
    functions_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    D3DVE_THROW_IF_NVENC_FAILED_MSG(createInstance(&functions_), "NvEncodeAPICreateInstance");
}


namespace {

GUID QueryCodecGuid(VideoCodec codec, std::string& error) {
    switch (codec) {
    case VideoCodec::H264: return NV_ENC_CODEC_H264_GUID;
    case VideoCodec::HEVC: return NV_ENC_CODEC_HEVC_GUID;
    case VideoCodec::AV1:
#ifdef NV_ENC_CODEC_AV1_GUID
        return NV_ENC_CODEC_AV1_GUID;
#else
        error = "This nvEncodeAPI.h does not define AV1 encode support.";
        return GUID_NULL;
#endif
    default:
        error = "Unsupported NVENC codec.";
        return GUID_NULL;
    }
}

NV_ENC_BUFFER_FORMAT QueryBufferFormat(VideoPixelFormat format, std::string& error) {
    switch (format) {
    case VideoPixelFormat::NV12: return NV_ENC_BUFFER_FORMAT_NV12;
    case VideoPixelFormat::P010: return NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
    default:
        error = "NVENC capability query requires NV12 or P010 input format.";
        return static_cast<NV_ENC_BUFFER_FORMAT>(0);
    }
}

bool GuidEqual(const GUID& a, const GUID& b) noexcept {
    return IsEqualGUID(a, b) != FALSE;
}

bool EnvEnabled(const char* name) noexcept {
    char value[16] = {};
    const DWORD length = GetEnvironmentVariableA(name, value, static_cast<DWORD>(std::size(value)));
    if (length == 0) return false;
    return value[0] != '0';
}

} // namespace

NvencFormatCapability QueryNvencDeviceSupport(void* device, NV_ENC_DEVICE_TYPE deviceType, VideoCodec codec, VideoPixelFormat inputFormat) {
    NvencFormatCapability result;
    result.codec = codec;
    result.inputFormat = inputFormat;

    std::string error;
    const GUID codecGuid = QueryCodecGuid(codec, error);
    const NV_ENC_BUFFER_FORMAT bufferFormat = QueryBufferFormat(inputFormat, error);
    if (!error.empty()) {
        result.message = error;
        return result;
    }
    if (!device) {
        result.message = "Null D3D device.";
        return result;
    }

    NvencApi api;
    try {
        api.load();
        result.runtimeAvailable = true;
    } catch (const std::exception& e) {
        result.message = e.what();
        return result;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams = {};
    openParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    openParams.device = device;
    openParams.deviceType = deviceType;
    openParams.apiVersion = NVENCAPI_VERSION;
    void* encoder = nullptr;
    const NVENCSTATUS openStatus = api.functions().nvEncOpenEncodeSessionEx(&openParams, &encoder);
    if (openStatus != NV_ENC_SUCCESS || !encoder) {
        result.message = std::string("nvEncOpenEncodeSessionEx failed: ") + NvencStatusToString(openStatus);
        return result;
    }
    result.deviceSupported = true;

    try {
        uint32_t guidCount = 0;
        D3DVE_THROW_IF_NVENC_FAILED_MSG(api.functions().nvEncGetEncodeGUIDCount(encoder, &guidCount), "NVENC capability query: nvEncGetEncodeGUIDCount");
        std::vector<GUID> guids(guidCount);
        uint32_t writtenGuids = 0;
        if (guidCount > 0) {
            D3DVE_THROW_IF_NVENC_FAILED_MSG(api.functions().nvEncGetEncodeGUIDs(encoder, guids.data(), guidCount, &writtenGuids), "NVENC capability query: nvEncGetEncodeGUIDs");
        }
        result.codecSupported = std::any_of(guids.begin(), guids.begin() + writtenGuids, [&](const GUID& g) { return GuidEqual(g, codecGuid); });
        if (!result.codecSupported) {
            result.message = "NVENC codec is not supported on this device.";
            api.functions().nvEncDestroyEncoder(encoder);
            return result;
        }

        uint32_t formatCount = 0;
        D3DVE_THROW_IF_NVENC_FAILED_MSG(api.functions().nvEncGetInputFormatCount(encoder, codecGuid, &formatCount), "NVENC capability query: nvEncGetInputFormatCount");
        std::vector<NV_ENC_BUFFER_FORMAT> formats(formatCount);
        uint32_t writtenFormats = 0;
        if (formatCount > 0) {
            D3DVE_THROW_IF_NVENC_FAILED_MSG(api.functions().nvEncGetInputFormats(encoder, codecGuid, formats.data(), formatCount, &writtenFormats), "NVENC capability query: nvEncGetInputFormats");
        }
        result.inputFormatSupported = std::any_of(formats.begin(), formats.begin() + writtenFormats, [&](NV_ENC_BUFFER_FORMAT f) { return f == bufferFormat; });
        if (!result.inputFormatSupported) {
            result.message = "NVENC input format is not supported for this codec on this device.";
            api.functions().nvEncDestroyEncoder(encoder);
            return result;
        }

        NV_ENC_CAPS_PARAM caps = {};
        caps.version = NV_ENC_CAPS_PARAM_VER;
        int value = 0;
        caps.capsToQuery = NV_ENC_CAPS_WIDTH_MAX;
        if (api.functions().nvEncGetEncodeCaps(encoder, codecGuid, &caps, &value) == NV_ENC_SUCCESS && value > 0) {
            result.maxWidth = static_cast<uint32_t>(value);
        }
        caps.capsToQuery = NV_ENC_CAPS_HEIGHT_MAX;
        if (api.functions().nvEncGetEncodeCaps(encoder, codecGuid, &caps, &value) == NV_ENC_SUCCESS && value > 0) {
            result.maxHeight = static_cast<uint32_t>(value);
        }
        result.supported = true;
        result.message = "NVENC codec/input format is supported.";
    } catch (const std::exception& e) {
        result.message = e.what();
    }

    api.functions().nvEncDestroyEncoder(encoder);
    return result;
}

NvencEncoderSession::NvencEncoderSession() = default;

NvencEncoderSession::~NvencEncoderSession() {
    try {
        close();
    } catch (...) {
    }
}

GUID NvencEncoderSession::codecGuid() const {
    switch (desc_.codec) {
    case VideoCodec::H264: return NV_ENC_CODEC_H264_GUID;
    case VideoCodec::HEVC: return NV_ENC_CODEC_HEVC_GUID;
    case VideoCodec::AV1:
#ifdef NV_ENC_CODEC_AV1_GUID
        return NV_ENC_CODEC_AV1_GUID;
#else
        throw D3DVideoEncoderError("This nvEncodeAPI.h does not define AV1 encode support.");
#endif
    default: throw D3DVideoEncoderError("Unsupported NVENC codec.");
    }
}

GUID NvencEncoderSession::profileGuid() const {
    switch (desc_.codec) {
    case VideoCodec::H264: return NV_ENC_H264_PROFILE_HIGH_GUID;
    case VideoCodec::HEVC:
        return desc_.inputFormat == VideoPixelFormat::P010
            ? NV_ENC_HEVC_PROFILE_MAIN10_GUID
            : NV_ENC_HEVC_PROFILE_MAIN_GUID;
    case VideoCodec::AV1:
#ifdef NV_ENC_AV1_PROFILE_MAIN_GUID
        return NV_ENC_AV1_PROFILE_MAIN_GUID;
#else
        throw D3DVideoEncoderError("This nvEncodeAPI.h does not define AV1 profile support.");
#endif
    default: throw D3DVideoEncoderError("Unsupported NVENC codec profile.");
    }
}

GUID NvencEncoderSession::presetGuid() const {
#if defined(NVENCAPI_MAJOR_VERSION) && (NVENCAPI_MAJOR_VERSION >= 10)
    // Video Codec SDK 10+ uses the P1-P7 preset GUIDs.  These GUIDs are
    // static GUID constants rather than preprocessor macros, so #ifdef on
    // NV_ENC_PRESET_P5_GUID is not reliable with newer SDKs.
    return NV_ENC_PRESET_P5_GUID;
#else
    // Legacy SDK fallback. Older headers define the default preset GUID.
    return NV_ENC_PRESET_DEFAULT_GUID;
#endif
}

NV_ENC_BUFFER_FORMAT NvencEncoderSession::bufferFormat() const {
    switch (desc_.inputFormat) {
    case VideoPixelFormat::NV12: return NV_ENC_BUFFER_FORMAT_NV12;
    case VideoPixelFormat::P010: return NV_ENC_BUFFER_FORMAT_YUV420_10BIT;
    default:
        throw D3DVideoEncoderError("NVENC backend requires inputFormat/internalFormat NV12 or P010. RGB conversion must be done before NVENC.");
    }
}

NV_ENC_PARAMS_RC_MODE NvencEncoderSession::rateControlMode() const {
    switch (desc_.rateControl) {
    case VideoRateControlMode::CBR: return NV_ENC_PARAMS_RC_CBR;
    case VideoRateControlMode::VBR: return NV_ENC_PARAMS_RC_VBR;
    case VideoRateControlMode::ConstantQP: return NV_ENC_PARAMS_RC_CONSTQP;
    case VideoRateControlMode::Quality: return NV_ENC_PARAMS_RC_VBR;
    default: return NV_ENC_PARAMS_RC_VBR;
    }
}

std::string NvencEncoderSession::describe() const {
    std::ostringstream oss;
    oss << "NVENC[codec=" << ToString(desc_.codec)
        << ", inputFormat=" << ToString(desc_.inputFormat)
        << ", size=" << desc_.width << "x" << desc_.height
        << ", fps=" << desc_.frameRateNum << "/" << desc_.frameRateDen
        << ", bitrate=" << desc_.bitrate << "]";
    return oss.str();
}

void NvencEncoderSession::trace(const std::string& message) const {
    if (traceEnabled_) {
        std::cerr << "[NVENC TRACE] " << message << std::endl;
    }
}

void NvencEncoderSession::initialize(
    void* device,
    NV_ENC_DEVICE_TYPE deviceType,
    NvencDirectXDeviceKind deviceKind,
    const NvencSessionDesc& desc) {
    if (!device) {
        throw D3DVideoEncoderError("NVENC initialize received a null D3D device.");
    }
    if (desc.outputPath.empty()) {
        throw D3DVideoEncoderError("NVENC outputPath is empty.");
    }
    if (desc.width == 0 || desc.height == 0) {
        throw D3DVideoEncoderError("NVENC width/height must be non-zero.");
    }
    if (desc.codec == VideoCodec::H264 && desc.inputFormat != VideoPixelFormat::NV12) {
        throw D3DVideoEncoderError("NVENC H.264 backend requires NV12 input.");
    }
    if (desc.inputFormat != VideoPixelFormat::NV12 && desc.inputFormat != VideoPixelFormat::P010) {
        throw D3DVideoEncoderError("NVENC backend requires NV12 or P010 input.");
    }
    if (!IsSupportedNvencBFrameCount(desc.bFrameCount)) {
        throw D3DVideoEncoderError("NVENC D3D11/D3D12 backends currently require bFrameCount=0.");
    }

    desc_ = desc;
    deviceKind_ = deviceKind;
    traceEnabled_ = EnvEnabled("D3DVE_NVENC_TRACE");
    internalAsync_ = deviceKind_ == NvencDirectXDeviceKind::D3D11 && EnvEnabled("D3DVE_NVENC_INTERNAL_ASYNC");
    sentFrameCount_ = 0;
    receivedPacketCount_ = 0;
    eosSent_ = false;
    trace(describe() + ": initialize begin internalAsync=" + std::string(internalAsync_ ? "true" : "false"));
    api_.load();

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams = {};
    openParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    openParams.device = device;
    openParams.deviceType = deviceType;
    openParams.apiVersion = NVENCAPI_VERSION;
    D3DVE_THROW_IF_NVENC_FAILED_MSG(api_.functions().nvEncOpenEncodeSessionEx(&openParams, &encoder_), describe() + ": nvEncOpenEncodeSessionEx");

    const GUID codec = codecGuid();
    const GUID preset = presetGuid();
    const NV_ENC_TUNING_INFO tuningInfo =
#if NVENCAPI_MAJOR_VERSION >= 11
        NV_ENC_TUNING_INFO_HIGH_QUALITY;
#else
        NV_ENC_TUNING_INFO_UNDEFINED;
#endif

    NV_ENC_PRESET_CONFIG presetConfig = {};
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
#if NVENCAPI_MAJOR_VERSION >= 11
    D3DVE_THROW_IF_NVENC_FAILED_MSG(
        api_.functions().nvEncGetEncodePresetConfigEx(encoder_, codec, preset, tuningInfo, &presetConfig),
        describe() + ": nvEncGetEncodePresetConfigEx");
#else
    D3DVE_THROW_IF_NVENC_FAILED_MSG(
        api_.functions().nvEncGetEncodePresetConfig(encoder_, codec, preset, &presetConfig),
        describe() + ": nvEncGetEncodePresetConfig");
#endif

    NV_ENC_CONFIG encodeConfig = presetConfig.presetCfg;
    encodeConfig.version = NV_ENC_CONFIG_VER;
    encodeConfig.rcParams.version = NV_ENC_RC_PARAMS_VER;
    encodeConfig.profileGUID = profileGuid();
    encodeConfig.gopLength = desc_.gopLength;
    encodeConfig.frameIntervalP = desc_.bFrameCount + 1;
    encodeConfig.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    encodeConfig.mvPrecision = NV_ENC_MV_PRECISION_DEFAULT;
    encodeConfig.rcParams.rateControlMode = rateControlMode();
    encodeConfig.rcParams.averageBitRate = desc_.bitrate;
    encodeConfig.rcParams.maxBitRate = desc_.bitrate;
    encodeConfig.rcParams.vbvBufferSize = desc_.bitrate;
    encodeConfig.rcParams.vbvInitialDelay = desc_.bitrate;
    encodeConfig.rcParams.constQP.qpInterP = 20;
    encodeConfig.rcParams.constQP.qpInterB = 22;
    encodeConfig.rcParams.constQP.qpIntra = 18;

    if (desc_.codec == VideoCodec::H264) {
        encodeConfig.encodeCodecConfig.h264Config.repeatSPSPPS = 1;
        encodeConfig.encodeCodecConfig.h264Config.idrPeriod = desc_.gopLength;
        encodeConfig.encodeCodecConfig.h264Config.chromaFormatIDC = 1;
        encodeConfig.encodeCodecConfig.h264Config.inputBitDepth = NV_ENC_BIT_DEPTH_8;
        encodeConfig.encodeCodecConfig.h264Config.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    } else if (desc_.codec == VideoCodec::HEVC) {
        encodeConfig.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
        encodeConfig.encodeCodecConfig.hevcConfig.idrPeriod = desc_.gopLength;
        encodeConfig.encodeCodecConfig.hevcConfig.inputBitDepth = (desc_.inputFormat == VideoPixelFormat::P010)
            ? NV_ENC_BIT_DEPTH_10
            : NV_ENC_BIT_DEPTH_8;
        encodeConfig.encodeCodecConfig.hevcConfig.outputBitDepth = encodeConfig.encodeCodecConfig.hevcConfig.inputBitDepth;
#ifdef NV_ENC_CONFIG_HEVC_VUI_PARAMETERS_VER
        (void)0;
#endif
    } else if (desc_.codec == VideoCodec::AV1) {
#ifdef NV_ENC_CODEC_AV1_GUID
        // Minimal AV1 initialization. Detailed AV1 tuning is intentionally left to a later pass.
#else
        throw D3DVideoEncoderError("NVENC AV1 is not available in this Video Codec SDK header.");
#endif
    }

    NV_ENC_INITIALIZE_PARAMS init = {};
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = codec;
    init.presetGUID = preset;
    init.encodeWidth = desc_.width;
    init.encodeHeight = desc_.height;
    init.darWidth = desc_.width;
    init.darHeight = desc_.height;
    init.frameRateNum = desc_.frameRateNum;
    init.frameRateDen = desc_.frameRateDen;
    init.enablePTD = 1;
    init.enableEncodeAsync = internalAsync_ ? 1u : 0u;
    init.enableOutputInVidmem = 0;
    init.reportSliceOffsets = 0;
    init.enableSubFrameWrite = 0;
    init.maxEncodeWidth = desc_.width;
    init.maxEncodeHeight = desc_.height;
#if NVENCAPI_MAJOR_VERSION >= 11
    init.tuningInfo = tuningInfo;
#endif
    init.bufferFormat = bufferFormat();
    init.encodeConfig = &encodeConfig;

    trace(describe() + ": nvEncInitializeEncoder begin enableEncodeAsync=" + std::to_string(init.enableEncodeAsync));
    const NVENCSTATUS initStatus = api_.functions().nvEncInitializeEncoder(encoder_, &init);
    trace(describe() + ": nvEncInitializeEncoder end status=" + NvencStatusToString(initStatus));
    if (initStatus != NV_ENC_SUCCESS) {
        std::ostringstream message;
        message << describe() << ": nvEncInitializeEncoder";
        if (api_.functions().nvEncGetLastErrorString) {
            if (const char* lastError = api_.functions().nvEncGetLastErrorString(encoder_)) {
                message << " lastError=\"" << lastError << "\"";
            }
        }
        ThrowNvenc(initStatus, "api_.functions().nvEncInitializeEncoder(encoder_, &init)", __FILE__, __LINE__, message.str());
    }

    if (internalAsync_) {
        registerAsyncEvent();
    }

    muxer_.open(desc_.outputPath, desc_.width, desc_.height, desc_.frameRateNum, desc_.frameRateDen, desc_.codec);

    if (deviceKind_ == NvencDirectXDeviceKind::D3D12) {
        // SDK sample buffer count: frameIntervalP + lookaheadDepth + extraOutputDelay.
        // This backend does not enable temporal filtering and uses the sample's default extra delay of 3.
        const uint32_t ringSize = std::max<uint32_t>(
            4u,
            encodeConfig.frameIntervalP + encodeConfig.rcParams.lookaheadDepth + 3u);
        d3d12Output_ = std::make_unique<NvencD3D12OutputStrategy>(ringSize);
        d3d12Output_->initialize(
            static_cast<ID3D12Device*>(device),
            encoder_,
            &api_.functions(),
            desc_.width,
            desc_.height,
            desc_.inputFormat,
            traceEnabled_);
    } else {
        createBitstreamBuffer();
    }
    trace(describe() + ": initialize complete");
}

void NvencEncoderSession::createBitstreamBuffer() {
    NV_ENC_CREATE_BITSTREAM_BUFFER create = {};
    create.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    trace(describe() + ": nvEncCreateBitstreamBuffer begin");
    const NVENCSTATUS status = api_.functions().nvEncCreateBitstreamBuffer(encoder_, &create);
    trace(describe() + ": nvEncCreateBitstreamBuffer end status=" + NvencStatusToString(status) + " buffer=" + std::to_string(reinterpret_cast<uintptr_t>(create.bitstreamBuffer)));
    D3DVE_THROW_IF_NVENC_FAILED_MSG(status, describe() + ": nvEncCreateBitstreamBuffer");
    bitstreamBuffer_ = create.bitstreamBuffer;
}

void NvencEncoderSession::destroyBitstreamBuffer() noexcept {
    if (encoder_ && bitstreamBuffer_) {
        trace(describe() + ": nvEncDestroyBitstreamBuffer begin");
        api_.functions().nvEncDestroyBitstreamBuffer(encoder_, bitstreamBuffer_);
        trace(describe() + ": nvEncDestroyBitstreamBuffer end");
        bitstreamBuffer_ = nullptr;
    }
}

void NvencEncoderSession::registerAsyncEvent() {
    if (completionEvent_) return;
    completionEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!completionEvent_) {
        throw D3DVideoEncoderError("NVENC internal async diagnostic failed to create completion event.");
    }
    NV_ENC_EVENT_PARAMS eventParams = {};
    eventParams.version = NV_ENC_EVENT_PARAMS_VER;
    eventParams.completionEvent = completionEvent_;
    trace(describe() + ": nvEncRegisterAsyncEvent begin event=" + std::to_string(reinterpret_cast<uintptr_t>(completionEvent_)));
    const NVENCSTATUS status = api_.functions().nvEncRegisterAsyncEvent(encoder_, &eventParams);
    trace(describe() + ": nvEncRegisterAsyncEvent end status=" + NvencStatusToString(status));
    D3DVE_THROW_IF_NVENC_FAILED_MSG(status, describe() + ": nvEncRegisterAsyncEvent");
    completionEventRegistered_ = true;
}

void NvencEncoderSession::unregisterAsyncEvent() noexcept {
    if (encoder_ && completionEvent_ && completionEventRegistered_) {
        NV_ENC_EVENT_PARAMS eventParams = {};
        eventParams.version = NV_ENC_EVENT_PARAMS_VER;
        eventParams.completionEvent = completionEvent_;
        trace(describe() + ": nvEncUnregisterAsyncEvent begin");
        api_.functions().nvEncUnregisterAsyncEvent(encoder_, &eventParams);
        trace(describe() + ": nvEncUnregisterAsyncEvent end");
        completionEventRegistered_ = false;
    }
    if (completionEvent_) {
        CloseHandle(completionEvent_);
        completionEvent_ = nullptr;
    }
}

bool NvencEncoderSession::waitForAsyncCompletion(const char* operation, uint32_t frameIndex) {
    if (!internalAsync_) return true;
    trace(describe() + ": completion event wait start operation=" + std::string(operation ? operation : "(unknown)") +
          " frame=" + std::to_string(frameIndex));
    const DWORD wait = WaitForSingleObject(completionEvent_, 30000);
    trace(describe() + ": completion event wait end result=" + std::to_string(wait) +
          " operation=" + std::string(operation ? operation : "(unknown)") +
          " frame=" + std::to_string(frameIndex));
    if (wait == WAIT_OBJECT_0) return true;
    if (wait == WAIT_TIMEOUT) {
        throw D3DVideoEncoderError("NVENC internal async completion event timed out.");
    }
    throw D3DVideoEncoderError("NVENC internal async completion event wait failed.");
}

NvencEncoderSession::RegisteredInputResource& NvencEncoderSession::getOrRegisterResource(void* resource) {
    for (auto& entry : registeredResources_) {
        if (entry.resourceKey == resource) {
            return entry;
        }
    }

    NV_ENC_REGISTER_RESOURCE reg = {};
    reg.version = NV_ENC_REGISTER_RESOURCE_VER;
    reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    reg.resourceToRegister = resource;
    reg.width = desc_.width;
    reg.height = desc_.height;
    reg.pitch = 0;
    reg.bufferFormat = bufferFormat();
    reg.bufferUsage = NV_ENC_INPUT_IMAGE;
    NV_ENC_FENCE_POINT_D3D12 d3d12RegistrationFence = {};
    if (deviceKind_ == NvencDirectXDeviceKind::D3D12) {
        if (!d3d12Output_) {
            throw D3DVideoEncoderError("NVENC D3D12 output strategy is unavailable during input registration.");
        }
        d3d12RegistrationFence = d3d12Output_->beginInputRegistration();
        reg.pInputFencePoint = &d3d12RegistrationFence;
    }
    trace(describe() + ": nvEncRegisterResource begin resource=" + std::to_string(reinterpret_cast<uintptr_t>(resource)));
    const NVENCSTATUS status = api_.functions().nvEncRegisterResource(encoder_, &reg);
    trace(describe() + ": nvEncRegisterResource end status=" + NvencStatusToString(status) +
          " registered=" + std::to_string(reinterpret_cast<uintptr_t>(reg.registeredResource)));
    D3DVE_THROW_IF_NVENC_FAILED_MSG(status, describe() + ": nvEncRegisterResource");
    if (deviceKind_ == NvencDirectXDeviceKind::D3D12) {
        d3d12Output_->waitForInputRegistration(d3d12RegistrationFence.signalValue);
    }

    RegisteredInputResource entry;
    entry.resourceKey = resource;
    entry.registeredResource = reg.registeredResource;

    // ID3D11Texture2D and ID3D12Resource are COM interfaces. Keep an IUnknown
    // reference so resources queued asynchronously or reused by the NVENC pool
    // cannot be destroyed while still registered with NVENC.
    auto* unknown = reinterpret_cast<IUnknown*>(resource);
    if (unknown) {
        (void)unknown->QueryInterface(IID_PPV_ARGS(entry.keepAlive.GetAddressOf()));
    }

    registeredResources_.push_back(std::move(entry));
    return registeredResources_.back();
}

void NvencEncoderSession::unregisterAllResources() noexcept {
    if (encoder_) {
        for (auto& entry : registeredResources_) {
            if (entry.registeredResource) {
                trace(describe() + ": nvEncUnregisterResource begin registered=" + std::to_string(reinterpret_cast<uintptr_t>(entry.registeredResource)));
                api_.functions().nvEncUnregisterResource(encoder_, entry.registeredResource);
                trace(describe() + ": nvEncUnregisterResource end");
                entry.registeredResource = nullptr;
            }
            entry.keepAlive.Reset();
        }
    }
    registeredResources_.clear();
}

void NvencEncoderSession::writeBitstream(void* bitstreamBuffer) {
    NV_ENC_LOCK_BITSTREAM lock = {};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = bitstreamBuffer;
    lock.doNotWait = 0;
    trace(describe() + ": nvEncLockBitstream start slot=0 received=" + std::to_string(receivedPacketCount_));
    const NVENCSTATUS lockStatus = api_.functions().nvEncLockBitstream(encoder_, &lock);
    trace(describe() + ": nvEncLockBitstream end status=" + NvencStatusToString(lockStatus) +
          " bytes=" + std::to_string(lock.bitstreamSizeInBytes));
    D3DVE_THROW_IF_NVENC_FAILED_MSG(lockStatus, describe() + ": nvEncLockBitstream");

    try {
        if (lock.bitstreamBufferPtr && lock.bitstreamSizeInBytes > 0) {
            muxer_.writeAccessUnit(
                static_cast<const uint8_t*>(lock.bitstreamBufferPtr),
                static_cast<size_t>(lock.bitstreamSizeInBytes),
                currentTimestamp100ns_,
                currentDuration100ns_);
        }
    } catch (...) {
        trace(describe() + ": nvEncUnlockBitstream begin after writer exception");
        api_.functions().nvEncUnlockBitstream(encoder_, bitstreamBuffer);
        trace(describe() + ": nvEncUnlockBitstream end after writer exception");
        throw;
    }

    trace(describe() + ": nvEncUnlockBitstream begin");
    const NVENCSTATUS unlockStatus = api_.functions().nvEncUnlockBitstream(encoder_, bitstreamBuffer);
    trace(describe() + ": nvEncUnlockBitstream end status=" + NvencStatusToString(unlockStatus));
    D3DVE_THROW_IF_NVENC_FAILED_MSG(unlockStatus, describe() + ": nvEncUnlockBitstream");
    ++receivedPacketCount_;
}

void NvencEncoderSession::encodeDirectXResource(void* resource, int64_t timestamp100ns, int64_t duration100ns) {
    if (!encoder_) {
        throw D3DVideoEncoderError("NVENC encode called before initialize.");
    }
    if (!resource) {
        throw D3DVideoEncoderError("NVENC encode received a null resource.");
    }

    if (deviceKind_ == NvencDirectXDeviceKind::D3D12) {
        encodeD3D12Resource(resource, timestamp100ns, duration100ns);
        return;
    }

    auto& registered = getOrRegisterResource(resource);
    void* mappedResource = nullptr;

    try {
        NV_ENC_MAP_INPUT_RESOURCE map = {};
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = registered.registeredResource;
        trace(describe() + ": nvEncMapInputResource begin frame=" + std::to_string(sentFrameCount_) +
              " registered=" + std::to_string(reinterpret_cast<uintptr_t>(registered.registeredResource)));
        const NVENCSTATUS mapStatus = api_.functions().nvEncMapInputResource(encoder_, &map);
        trace(describe() + ": nvEncMapInputResource end status=" + NvencStatusToString(mapStatus) +
              " mapped=" + std::to_string(reinterpret_cast<uintptr_t>(map.mappedResource)));
        D3DVE_THROW_IF_NVENC_FAILED_MSG(mapStatus, describe() + ": nvEncMapInputResource");
        mappedResource = map.mappedResource;

        NV_ENC_PIC_PARAMS pic = {};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputBuffer = mappedResource;
        pic.bufferFmt = bufferFormat();
        pic.inputWidth = desc_.width;
        pic.inputHeight = desc_.height;
        pic.outputBitstream = bitstreamBuffer_;
        pic.completionEvent = internalAsync_ ? completionEvent_ : nullptr;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic.inputTimeStamp = static_cast<uint64_t>(timestamp100ns);
        (void)duration100ns;

        currentTimestamp100ns_ = timestamp100ns;
        currentDuration100ns_ = duration100ns;
        trace(describe() + ": nvEncEncodePicture begin frame=" + std::to_string(sentFrameCount_) +
              " slot=0 internalAsync=" + std::string(internalAsync_ ? "true" : "false"));
        const NVENCSTATUS encodeStatus = api_.functions().nvEncEncodePicture(encoder_, &pic);
        trace(describe() + ": nvEncEncodePicture end frame=" + std::to_string(sentFrameCount_) +
              " status=" + NvencStatusToString(encodeStatus));
        if (encodeStatus != NV_ENC_SUCCESS && encodeStatus != NV_ENC_ERR_NEED_MORE_INPUT) {
            ThrowNvenc(encodeStatus, "nvEncEncodePicture", __FILE__, __LINE__, describe());
        }
        ++sentFrameCount_;
        if (encodeStatus == NV_ENC_SUCCESS) {
            waitForAsyncCompletion("encode", static_cast<uint32_t>(sentFrameCount_ - 1));
            writeBitstream(bitstreamBuffer_);
        }

        trace(describe() + ": nvEncUnmapInputResource begin frame=" + std::to_string(sentFrameCount_ - 1));
        const NVENCSTATUS unmapStatus = api_.functions().nvEncUnmapInputResource(encoder_, mappedResource);
        trace(describe() + ": nvEncUnmapInputResource end status=" + NvencStatusToString(unmapStatus));
        D3DVE_THROW_IF_NVENC_FAILED_MSG(unmapStatus, describe() + ": nvEncUnmapInputResource");
        mappedResource = nullptr;
    } catch (...) {
        if (mappedResource) {
            trace(describe() + ": nvEncUnmapInputResource begin during exception");
            api_.functions().nvEncUnmapInputResource(encoder_, mappedResource);
            trace(describe() + ": nvEncUnmapInputResource end during exception");
        }
        throw;
    }
}

void NvencEncoderSession::encodeD3D12Resource(void* resource, int64_t timestamp100ns, int64_t duration100ns) {
    if (!d3d12Output_) {
        throw D3DVideoEncoderError("NVENC D3D12 output strategy is not initialized.");
    }

    auto& registered = getOrRegisterResource(resource);
    const auto prepared = d3d12Output_->prepare(
        registered.registeredResource,
        sentFrameCount_,
        timestamp100ns,
        duration100ns);

    try {
        NV_ENC_PIC_PARAMS pic = {};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputBuffer = prepared.input;
        pic.bufferFmt = bufferFormat();
        pic.inputWidth = desc_.width;
        pic.inputHeight = desc_.height;
        pic.outputBitstream = prepared.output;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic.inputTimeStamp = static_cast<uint64_t>(timestamp100ns);

        trace(describe() + ": D3D12 nvEncEncodePicture begin frame=" + std::to_string(sentFrameCount_) +
              " slot=" + std::to_string(prepared.slotIndex));
        const NVENCSTATUS status = api_.functions().nvEncEncodePicture(encoder_, &pic);
        trace(describe() + ": D3D12 nvEncEncodePicture end frame=" + std::to_string(sentFrameCount_) +
              " status=" + NvencStatusToString(status));
        if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT) {
            std::string message = describe();
            if (api_.functions().nvEncGetLastErrorString) {
                if (const char* lastError = api_.functions().nvEncGetLastErrorString(encoder_)) {
                    message += std::string(": ") + lastError;
                }
            }
            ThrowNvenc(status, "nvEncEncodePicture", __FILE__, __LINE__, message);
        }

        d3d12Output_->commit(prepared);
        ++sentFrameCount_;
        if (status == NV_ENC_SUCCESS) {
            d3d12Output_->drainNext(muxer_);
            receivedPacketCount_ = d3d12Output_->receivedCount();
        }
    } catch (...) {
        if (prepared.slotIndex < d3d12Output_->slotCount() &&
            d3d12Output_->slotState(prepared.slotIndex) == NvencD3D12OutputStrategy::SlotState::Prepared) {
            d3d12Output_->rollback(prepared);
        }
        throw;
    }
}

void NvencEncoderSession::flush() {
    if (!encoder_ || eosSent_) return;

    if (deviceKind_ == NvencDirectXDeviceKind::D3D12) {
        flushD3D12();
        return;
    }

    NV_ENC_PIC_PARAMS eos = {};
    eos.version = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    eos.outputBitstream = bitstreamBuffer_;
    eos.completionEvent = internalAsync_ ? completionEvent_ : nullptr;
    trace(describe() + ": nvEncEncodePicture(EOS) begin sent=" + std::to_string(sentFrameCount_) +
          " received=" + std::to_string(receivedPacketCount_));
    const NVENCSTATUS status = api_.functions().nvEncEncodePicture(encoder_, &eos);
    trace(describe() + ": nvEncEncodePicture(EOS) end status=" + NvencStatusToString(status));
    if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT) {
        ThrowNvenc(status, "nvEncEncodePicture(EOS)", __FILE__, __LINE__, describe());
    }
    if (ShouldDrainNvencEos(status, sentFrameCount_, receivedPacketCount_)) {
        waitForAsyncCompletion("EOS", static_cast<uint32_t>(sentFrameCount_));
        writeBitstream(bitstreamBuffer_);
    } else if (status == NV_ENC_SUCCESS) {
        trace(describe() + ": EOS produced no pending packet; skip nvEncLockBitstream sent=" +
              std::to_string(sentFrameCount_) + " received=" + std::to_string(receivedPacketCount_));
    }
    eosSent_ = true;
    trace(describe() + ": muxer flush begin");
    muxer_.flush();
    trace(describe() + ": muxer flush end");
}

void NvencEncoderSession::flushD3D12() {
    if (!d3d12Output_) {
        throw D3DVideoEncoderError("NVENC D3D12 output strategy is not initialized during flush.");
    }

    NV_ENC_PIC_PARAMS eos = {};
    eos.version = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    trace(describe() + ": D3D12 nvEncEncodePicture(EOS) begin sent=" + std::to_string(sentFrameCount_) +
          " received=" + std::to_string(receivedPacketCount_));
    const NVENCSTATUS status = api_.functions().nvEncEncodePicture(encoder_, &eos);
    trace(describe() + ": D3D12 nvEncEncodePicture(EOS) end status=" + NvencStatusToString(status));
    if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT) {
        ThrowNvenc(status, "nvEncEncodePicture(EOS)", __FILE__, __LINE__, describe());
    }

    if (d3d12Output_->outstandingCount() > 0) {
        d3d12Output_->drainAll(muxer_);
        receivedPacketCount_ = d3d12Output_->receivedCount();
    } else if (status == NV_ENC_SUCCESS) {
        trace(describe() + ": D3D12 EOS produced no pending packet; skip nvEncLockBitstream sent=" +
              std::to_string(sentFrameCount_) + " received=" + std::to_string(receivedPacketCount_));
    }

    eosSent_ = true;
    trace(describe() + ": muxer flush begin");
    muxer_.flush();
    trace(describe() + ": muxer flush end");
}

void NvencEncoderSession::close() {
    if (!encoder_) return;

    std::exception_ptr pending = nullptr;
    try {
        flush();
    } catch (...) {
        pending = std::current_exception();
    }

    trace(describe() + ": close begin");
    if (d3d12Output_) {
        d3d12Output_->release();
        d3d12Output_.reset();
    }
    destroyBitstreamBuffer();
    unregisterAllResources();
    unregisterAsyncEvent();

    if (encoder_) {
        trace(describe() + ": nvEncDestroyEncoder begin");
        api_.functions().nvEncDestroyEncoder(encoder_);
        trace(describe() + ": nvEncDestroyEncoder end");
        encoder_ = nullptr;
    }
    trace(describe() + ": muxer close begin");
    muxer_.close();
    trace(describe() + ": muxer close end");

    if (pending) {
        std::rethrow_exception(pending);
    }
}

} // namespace D3DVideoEncoderLib
