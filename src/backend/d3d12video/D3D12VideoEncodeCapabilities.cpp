#include "backend/d3d12video/D3D12VideoEncodeCapabilities.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

#include <d3d12video.h>
#include <wrl/client.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <vector>

namespace D3DVideoEncoderLib {
namespace {

using Microsoft::WRL::ComPtr;

std::string hr_hex(HRESULT hr) {
    std::ostringstream os;
    os << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return os.str();
}

D3D12_VIDEO_ENCODER_CODEC to_d3d12_codec(VideoCodec codec, std::string& message) {
    switch (codec) {
    case VideoCodec::H264: return D3D12_VIDEO_ENCODER_CODEC_H264;
    case VideoCodec::HEVC: return D3D12_VIDEO_ENCODER_CODEC_HEVC;
    case VideoCodec::AV1:
        message = "D3D12 Video Encode AV1 capability query is intentionally postponed in Phase 1.";
        return D3D12_VIDEO_ENCODER_CODEC_H264;
    default:
        message = "Unsupported VideoCodec for D3D12 Video Encode.";
        return D3D12_VIDEO_ENCODER_CODEC_H264;
    }
}

struct ProfileStorage {
    D3D12_VIDEO_ENCODER_PROFILE_H264 h264 = D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH;
    D3D12_VIDEO_ENCODER_PROFILE_HEVC hevc = D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN;
    D3D12_VIDEO_ENCODER_PROFILE_DESC desc = {};
};

ProfileStorage make_profile(VideoCodec codec, VideoPixelFormat inputFormat) {
    ProfileStorage s;
    switch (codec) {
    case VideoCodec::H264:
        s.h264 = D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH;
        s.desc.DataSize = sizeof(s.h264);
        s.desc.pH264Profile = &s.h264;
        break;
    case VideoCodec::HEVC:
        s.hevc = (inputFormat == VideoPixelFormat::P010)
            ? D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN10
            : D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN;
        s.desc.DataSize = sizeof(s.hevc);
        s.desc.pHEVCProfile = &s.hevc;
        break;
    default:
        s.h264 = D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH;
        s.desc.DataSize = sizeof(s.h264);
        s.desc.pH264Profile = &s.h264;
        break;
    }
    return s;
}

struct LevelStorage {
    D3D12_VIDEO_ENCODER_LEVELS_H264 h264Min = D3D12_VIDEO_ENCODER_LEVELS_H264_1;
    D3D12_VIDEO_ENCODER_LEVELS_H264 h264Max = D3D12_VIDEO_ENCODER_LEVELS_H264_52;
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC hevcMin = { D3D12_VIDEO_ENCODER_LEVELS_HEVC_1, D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN };
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC hevcMax = { D3D12_VIDEO_ENCODER_LEVELS_HEVC_52, D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN };
    D3D12_VIDEO_ENCODER_LEVEL_SETTING minDesc = {};
    D3D12_VIDEO_ENCODER_LEVEL_SETTING maxDesc = {};
};

LevelStorage make_level_storage(VideoCodec codec) {
    LevelStorage s;
    if (codec == VideoCodec::HEVC) {
        s.minDesc.DataSize = sizeof(s.hevcMin);
        s.minDesc.pHEVCLevelSetting = &s.hevcMin;
        s.maxDesc.DataSize = sizeof(s.hevcMax);
        s.maxDesc.pHEVCLevelSetting = &s.hevcMax;
    } else {
        s.minDesc.DataSize = sizeof(s.h264Min);
        s.minDesc.pH264LevelSetting = &s.h264Min;
        s.maxDesc.DataSize = sizeof(s.h264Max);
        s.maxDesc.pH264LevelSetting = &s.h264Max;
    }
    return s;
}

bool requested_resolution_in_range(
    const D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION& r,
    uint32_t width,
    uint32_t height) {

    if (!r.IsSupported) return false;
    if (width < r.MinResolutionSupported.Width || height < r.MinResolutionSupported.Height) return false;
    if (width > r.MaxResolutionSupported.Width || height > r.MaxResolutionSupported.Height) return false;
    if (r.ResolutionWidthMultipleRequirement != 0 && (width % r.ResolutionWidthMultipleRequirement) != 0) return false;
    if (r.ResolutionHeightMultipleRequirement != 0 && (height % r.ResolutionHeightMultipleRequirement) != 0) return false;
    return true;
}

void append_message(std::ostringstream& os, const std::string& s) {
    if (!s.empty()) {
        if (os.tellp() > 0) os << " ";
        os << s;
    }
}

} // namespace

D3D12VideoEncodeFormatCapability QueryD3D12VideoEncodeDeviceSupport(
    D3D12CoreLib::D3D12Core* core,
    VideoCodec codec,
    VideoPixelFormat inputFormat,
    uint32_t width,
    uint32_t height) {

    D3D12VideoEncodeFormatCapability result;
    result.codec = codec;
    result.inputFormat = inputFormat;
    result.requestedWidth = width;
    result.requestedHeight = height;

    if (!core) {
        result.message = "D3D12 Video Encode capability query requires a non-null D3D12Core.";
        return result;
    }
    if (width == 0 || height == 0) {
        result.message = "D3D12 Video Encode capability query requires non-zero width/height.";
        return result;
    }
    if ((width % 2) != 0 || (height % 2) != 0) {
        result.message = "D3D12 Video Encode capability query requires even width/height for NV12/P010.";
        return result;
    }
    if (!IsYuv420EncodeFormat(inputFormat)) {
        result.message = "D3D12 Video Encode Phase 1 queries only NV12/P010 input formats.";
        return result;
    }
    if (codec == VideoCodec::H264 && inputFormat != VideoPixelFormat::NV12) {
        result.message = "D3D12 Video Encode H.264 requires NV12 input in this implementation.";
        return result;
    }

    std::ostringstream message;
    std::string codecMessage;
    const D3D12_VIDEO_ENCODER_CODEC d3dCodec = to_d3d12_codec(codec, codecMessage);
    if (!codecMessage.empty()) {
        result.message = codecMessage;
        return result;
    }

    ComPtr<ID3D12VideoDevice> videoDevice;
    HRESULT hr = core->GetDevice()->QueryInterface(IID_PPV_ARGS(&videoDevice));
    result.queryHr = hr;
    if (FAILED(hr) || !videoDevice) {
        result.message = "ID3D12VideoDevice is not available. HRESULT=" + hr_hex(hr);
        return result;
    }
    result.videoDeviceAvailable = true;

    ComPtr<ID3D12VideoDevice3> videoDevice3;
    hr = core->GetDevice()->QueryInterface(IID_PPV_ARGS(&videoDevice3));
    if (SUCCEEDED(hr) && videoDevice3) {
        result.videoDevice3Available = true;
    } else {
        result.queryHr = hr;
        append_message(message, "ID3D12VideoDevice3 is not available. HRESULT=" + hr_hex(hr) + ".");
        result.message = message.str();
        return result;
    }

    D3D12_FEATURE_DATA_VIDEO_FEATURE_AREA_SUPPORT featureArea = {};
    featureArea.NodeIndex = 0;
    hr = videoDevice->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_FEATURE_AREA_SUPPORT,
        &featureArea,
        sizeof(featureArea));
    if (SUCCEEDED(hr) && featureArea.VideoEncodeSupport) {
        result.featureAreaSupported = true;
    } else {
        result.queryHr = hr;
        append_message(message, "D3D12 video encode feature area is not supported. HRESULT=" + hr_hex(hr) + ".");
        result.message = message.str();
        return result;
    }

    D3D12_FEATURE_DATA_VIDEO_ENCODER_CODEC codecData = {};
    codecData.NodeIndex = 0;
    codecData.Codec = d3dCodec;
    hr = videoDevice->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_CODEC,
        &codecData,
        sizeof(codecData));
    if (SUCCEEDED(hr) && codecData.IsSupported) {
        result.codecSupported = true;
    } else {
        result.queryHr = hr;
        append_message(message, std::string("D3D12 Video Encode codec is not supported: ") + ToString(codec) + ". HRESULT=" + hr_hex(hr) + ".");
        result.message = message.str();
        return result;
    }

    ProfileStorage profile = make_profile(codec, inputFormat);
    LevelStorage levels = make_level_storage(codec);

    D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL profileLevel = {};
    profileLevel.NodeIndex = 0;
    profileLevel.Codec = d3dCodec;
    profileLevel.Profile = profile.desc;
    profileLevel.MinSupportedLevel = levels.minDesc;
    profileLevel.MaxSupportedLevel = levels.maxDesc;
    hr = videoDevice->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL,
        &profileLevel,
        sizeof(profileLevel));
    if (SUCCEEDED(hr) && profileLevel.IsSupported) {
        result.profileSupported = true;
        levels.minDesc = profileLevel.MinSupportedLevel;
        levels.maxDesc = profileLevel.MaxSupportedLevel;
    } else {
        result.queryHr = hr;
        append_message(message, "D3D12 Video Encode profile is not supported. HRESULT=" + hr_hex(hr) + ".");
        result.message = message.str();
        return result;
    }

    D3D12_FEATURE_DATA_VIDEO_ENCODER_INPUT_FORMAT input = {};
    input.NodeIndex = 0;
    input.Codec = d3dCodec;
    input.Profile = profile.desc;
    input.Format = ToDxgiFormat(inputFormat);
    hr = videoDevice->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_INPUT_FORMAT,
        &input,
        sizeof(input));
    if (SUCCEEDED(hr) && input.IsSupported) {
        result.inputFormatSupported = true;
    } else {
        result.queryHr = hr;
        append_message(message, std::string("D3D12 Video Encode input format is not supported: ") + ToString(inputFormat) + ". HRESULT=" + hr_hex(hr) + ".");
        result.message = message.str();
        return result;
    }

    D3D12_FEATURE_DATA_VIDEO_ENCODER_RATE_CONTROL_MODE cbr = {};
    cbr.NodeIndex = 0;
    cbr.Codec = d3dCodec;
    cbr.RateControlMode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR;
    hr = videoDevice->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_RATE_CONTROL_MODE,
        &cbr,
        sizeof(cbr));
    result.cbrSupported = SUCCEEDED(hr) && cbr.IsSupported;

    D3D12_FEATURE_DATA_VIDEO_ENCODER_RATE_CONTROL_MODE cqp = {};
    cqp.NodeIndex = 0;
    cqp.Codec = d3dCodec;
    cqp.RateControlMode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
    const HRESULT cqpHr = videoDevice->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_RATE_CONTROL_MODE,
        &cqp,
        sizeof(cqp));
    result.cqpSupported = SUCCEEDED(cqpHr) && cqp.IsSupported;
    result.rateControlSupported = result.cbrSupported || result.cqpSupported;
    if (!result.rateControlSupported) {
        result.queryHr = FAILED(hr) ? hr : cqpHr;
        append_message(message, "D3D12 Video Encode neither CBR nor CQP rate control is supported.");
        result.message = message.str();
        return result;
    }

    D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION_RATIOS_COUNT ratioCount = {};
    ratioCount.NodeIndex = 0;
    ratioCount.Codec = d3dCodec;
    hr = videoDevice->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_OUTPUT_RESOLUTION_RATIOS_COUNT,
        &ratioCount,
        sizeof(ratioCount));
    if (FAILED(hr)) {
        result.queryHr = hr;
        append_message(message, "D3D12 Video Encode output resolution ratio count query failed. HRESULT=" + hr_hex(hr) + ".");
        result.message = message.str();
        return result;
    }

    std::vector<D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_RATIO_DESC> ratios(ratioCount.ResolutionRatiosCount);
    D3D12_FEATURE_DATA_VIDEO_ENCODER_OUTPUT_RESOLUTION outputResolution = {};
    outputResolution.NodeIndex = 0;
    outputResolution.Codec = d3dCodec;
    outputResolution.ResolutionRatiosCount = ratioCount.ResolutionRatiosCount;
    outputResolution.pResolutionRatios = ratios.empty() ? nullptr : ratios.data();
    hr = videoDevice->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_OUTPUT_RESOLUTION,
        &outputResolution,
        sizeof(outputResolution));
    if (FAILED(hr)) {
        result.queryHr = hr;
        append_message(message, "D3D12 Video Encode output resolution query failed. HRESULT=" + hr_hex(hr) + ".");
        result.message = message.str();
        return result;
    }

    result.minWidth = outputResolution.MinResolutionSupported.Width;
    result.minHeight = outputResolution.MinResolutionSupported.Height;
    result.maxWidth = outputResolution.MaxResolutionSupported.Width;
    result.maxHeight = outputResolution.MaxResolutionSupported.Height;
    result.widthMultiple = outputResolution.ResolutionWidthMultipleRequirement;
    result.heightMultiple = outputResolution.ResolutionHeightMultipleRequirement;
    result.outputResolutionSupported = requested_resolution_in_range(outputResolution, width, height);
    if (!result.outputResolutionSupported) {
        append_message(message, "D3D12 Video Encode output resolution does not support the requested size.");
        result.message = message.str();
        return result;
    }

    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC requestedResolution = { width, height };
    D3D12_VIDEO_ENCODER_HEAP_DESC heapDesc = {};
    heapDesc.NodeMask = 0;
    heapDesc.Flags = D3D12_VIDEO_ENCODER_HEAP_FLAG_NONE;
    heapDesc.EncodeCodec = d3dCodec;
    heapDesc.EncodeProfile = profile.desc;
    heapDesc.EncodeLevel = levels.maxDesc;
    heapDesc.ResolutionsListCount = 1;
    heapDesc.pResolutionList = &requestedResolution;

    D3D12_FEATURE_DATA_VIDEO_ENCODER_HEAP_SIZE heapSize = {};
    heapSize.HeapDesc = heapDesc;
    hr = videoDevice->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_HEAP_SIZE,
        &heapSize,
        sizeof(heapSize));
    if (SUCCEEDED(hr) && heapSize.IsSupported) {
        result.heapSizeSupported = true;
        result.heapMemoryPoolL0Size = heapSize.MemoryPoolL0Size;
        result.heapMemoryPoolL1Size = heapSize.MemoryPoolL1Size;
    } else {
        result.queryHr = hr;
        append_message(message, "D3D12 Video Encode encoder heap size query failed or is unsupported. HRESULT=" + hr_hex(hr) + ".");
        result.message = message.str();
        return result;
    }

    result.supported = result.videoDeviceAvailable &&
                       result.videoDevice3Available &&
                       result.featureAreaSupported &&
                       result.codecSupported &&
                       result.profileSupported &&
                       result.inputFormatSupported &&
                       result.rateControlSupported &&
                       result.outputResolutionSupported &&
                       result.heapSizeSupported;
    if (result.supported) {
        append_message(message, "D3D12 Video Encode support confirmed for Phase 1 capability/open path.");
    }
    result.message = message.str();
    return result;
}

D3D12VideoEncodeCapabilities QueryD3D12VideoEncodeDeviceCapabilities(
    D3D12CoreLib::D3D12Core* core,
    uint32_t width,
    uint32_t height) {

    D3D12VideoEncodeCapabilities caps;
    caps.h264Nv12 = QueryD3D12VideoEncodeDeviceSupport(core, VideoCodec::H264, VideoPixelFormat::NV12, width, height);
    caps.hevcNv12 = QueryD3D12VideoEncodeDeviceSupport(core, VideoCodec::HEVC, VideoPixelFormat::NV12, width, height);
    caps.hevcP010 = QueryD3D12VideoEncodeDeviceSupport(core, VideoCodec::HEVC, VideoPixelFormat::P010, width, height);
    return caps;
}

} // namespace D3DVideoEncoderLib
