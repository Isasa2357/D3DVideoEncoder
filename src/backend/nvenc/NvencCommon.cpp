#include "backend/nvenc/NvencCommon.hpp"

#include <exception>
#include <filesystem>
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
#ifdef NV_ENC_PRESET_P5_GUID
    return NV_ENC_PRESET_P5_GUID;
#else
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

void NvencEncoderSession::initialize(void* device, NV_ENC_DEVICE_TYPE deviceType, const NvencSessionDesc& desc) {
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

    desc_ = desc;
    api_.load();

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS openParams = {};
    openParams.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    openParams.device = device;
    openParams.deviceType = deviceType;
    openParams.apiVersion = NVENCAPI_VERSION;
    D3DVE_THROW_IF_NVENC_FAILED_MSG(api_.functions().nvEncOpenEncodeSessionEx(&openParams, &encoder_), describe() + ": nvEncOpenEncodeSessionEx");

    NV_ENC_CONFIG encodeConfig = {};
    encodeConfig.version = NV_ENC_CONFIG_VER;
    encodeConfig.profileGUID = profileGuid();
    encodeConfig.gopLength = desc_.gopLength;
    encodeConfig.frameIntervalP = desc_.bFrameCount + 1;
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
    } else if (desc_.codec == VideoCodec::HEVC) {
        encodeConfig.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
        encodeConfig.encodeCodecConfig.hevcConfig.idrPeriod = desc_.gopLength;
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
    init.encodeGUID = codecGuid();
    init.presetGUID = presetGuid();
    init.encodeWidth = desc_.width;
    init.encodeHeight = desc_.height;
    init.darWidth = desc_.width;
    init.darHeight = desc_.height;
    init.frameRateNum = desc_.frameRateNum;
    init.frameRateDen = desc_.frameRateDen;
    init.enablePTD = 1;
    init.reportSliceOffsets = 0;
    init.enableSubFrameWrite = 0;
#if NVENCAPI_MAJOR_VERSION >= 11
    init.tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
#endif
    init.encodeConfig = &encodeConfig;

    D3DVE_THROW_IF_NVENC_FAILED_MSG(api_.functions().nvEncInitializeEncoder(encoder_, &init), describe() + ": nvEncInitializeEncoder");

    output_.open(std::filesystem::path(desc_.outputPath), std::ios::binary | std::ios::trunc);
    if (!output_) {
        throw D3DVideoEncoderError(describe() + ": failed to open output elementary stream file.");
    }

    createBitstreamBuffer();
}

void NvencEncoderSession::createBitstreamBuffer() {
    NV_ENC_CREATE_BITSTREAM_BUFFER create = {};
    create.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    D3DVE_THROW_IF_NVENC_FAILED_MSG(api_.functions().nvEncCreateBitstreamBuffer(encoder_, &create), describe() + ": nvEncCreateBitstreamBuffer");
    bitstreamBuffer_ = create.bitstreamBuffer;
}

void NvencEncoderSession::destroyBitstreamBuffer() noexcept {
    if (encoder_ && bitstreamBuffer_) {
        api_.functions().nvEncDestroyBitstreamBuffer(encoder_, bitstreamBuffer_);
        bitstreamBuffer_ = nullptr;
    }
}

void NvencEncoderSession::writeBitstream(void* bitstreamBuffer) {
    NV_ENC_LOCK_BITSTREAM lock = {};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = bitstreamBuffer;
    lock.doNotWait = 0;
    D3DVE_THROW_IF_NVENC_FAILED_MSG(api_.functions().nvEncLockBitstream(encoder_, &lock), describe() + ": nvEncLockBitstream");

    try {
        if (lock.bitstreamBufferPtr && lock.bitstreamSizeInBytes > 0) {
            output_.write(static_cast<const char*>(lock.bitstreamBufferPtr), static_cast<std::streamsize>(lock.bitstreamSizeInBytes));
            if (!output_) {
                throw D3DVideoEncoderError(describe() + ": failed to write NVENC bitstream file.");
            }
        }
    } catch (...) {
        api_.functions().nvEncUnlockBitstream(encoder_, bitstreamBuffer);
        throw;
    }

    D3DVE_THROW_IF_NVENC_FAILED_MSG(api_.functions().nvEncUnlockBitstream(encoder_, bitstreamBuffer), describe() + ": nvEncUnlockBitstream");
}

void NvencEncoderSession::encodeDirectXResource(void* resource, int64_t timestamp100ns, int64_t duration100ns) {
    if (!encoder_) {
        throw D3DVideoEncoderError("NVENC encode called before initialize.");
    }
    if (!resource) {
        throw D3DVideoEncoderError("NVENC encode received a null resource.");
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
    D3DVE_THROW_IF_NVENC_FAILED_MSG(api_.functions().nvEncRegisterResource(encoder_, &reg), describe() + ": nvEncRegisterResource");

    void* registeredResource = reg.registeredResource;
    void* mappedResource = nullptr;

    try {
        NV_ENC_MAP_INPUT_RESOURCE map = {};
        map.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        map.registeredResource = registeredResource;
        D3DVE_THROW_IF_NVENC_FAILED_MSG(api_.functions().nvEncMapInputResource(encoder_, &map), describe() + ": nvEncMapInputResource");
        mappedResource = map.mappedResource;

        NV_ENC_PIC_PARAMS pic = {};
        pic.version = NV_ENC_PIC_PARAMS_VER;
        pic.inputBuffer = mappedResource;
        pic.bufferFmt = bufferFormat();
        pic.inputWidth = desc_.width;
        pic.inputHeight = desc_.height;
        pic.outputBitstream = bitstreamBuffer_;
        pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
        pic.inputTimeStamp = static_cast<uint64_t>(timestamp100ns);
        (void)duration100ns;

        const NVENCSTATUS encodeStatus = api_.functions().nvEncEncodePicture(encoder_, &pic);
        if (encodeStatus != NV_ENC_SUCCESS && encodeStatus != NV_ENC_ERR_NEED_MORE_INPUT) {
            ThrowNvenc(encodeStatus, "nvEncEncodePicture", __FILE__, __LINE__, describe());
        }
        if (encodeStatus == NV_ENC_SUCCESS) {
            writeBitstream(bitstreamBuffer_);
        }

        D3DVE_THROW_IF_NVENC_FAILED_MSG(api_.functions().nvEncUnmapInputResource(encoder_, mappedResource), describe() + ": nvEncUnmapInputResource");
        mappedResource = nullptr;
        D3DVE_THROW_IF_NVENC_FAILED_MSG(api_.functions().nvEncUnregisterResource(encoder_, registeredResource), describe() + ": nvEncUnregisterResource");
        registeredResource = nullptr;
    } catch (...) {
        if (mappedResource) {
            api_.functions().nvEncUnmapInputResource(encoder_, mappedResource);
        }
        if (registeredResource) {
            api_.functions().nvEncUnregisterResource(encoder_, registeredResource);
        }
        throw;
    }
}

void NvencEncoderSession::flush() {
    if (!encoder_ || eosSent_) return;

    NV_ENC_PIC_PARAMS eos = {};
    eos.version = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    const NVENCSTATUS status = api_.functions().nvEncEncodePicture(encoder_, &eos);
    if (status != NV_ENC_SUCCESS && status != NV_ENC_ERR_NEED_MORE_INPUT) {
        ThrowNvenc(status, "nvEncEncodePicture(EOS)", __FILE__, __LINE__, describe());
    }
    eosSent_ = true;
    if (output_) {
        output_.flush();
    }
}

void NvencEncoderSession::close() {
    if (!encoder_) return;

    std::exception_ptr pending = nullptr;
    try {
        flush();
    } catch (...) {
        pending = std::current_exception();
    }

    destroyBitstreamBuffer();

    if (encoder_) {
        api_.functions().nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;
    }
    if (output_) {
        output_.close();
    }

    if (pending) {
        std::rethrow_exception(pending);
    }
}

} // namespace D3DVideoEncoderLib
