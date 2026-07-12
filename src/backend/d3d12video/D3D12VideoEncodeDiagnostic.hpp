#pragma once

#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

#include <Windows.h>
#include <d3d12video.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <ostream>
#include <sstream>
#include <string>

#ifndef D3DVE_DIAGNOSTIC_WINDOWS_SDK_VERSION
#define D3DVE_DIAGNOSTIC_WINDOWS_SDK_VERSION "unknown"
#endif

namespace D3DVideoEncoderLib::Diagnostics {

struct NativeD3D12H264Nv12DiagnosticResult {
    D3D12VideoEncodeFormatCapability lightweight;
    HRESULT profileLevelHr = E_FAIL;
    BOOL profileLevelIsSupported = FALSE;
    HRESULT fullQueryUnbufferedHr = E_FAIL;
    HRESULT fullQueryBufferedHr = E_FAIL;
    D3D12_VIDEO_ENCODER_VALIDATION_FLAGS validationFlags =
        static_cast<D3D12_VIDEO_ENCODER_VALIDATION_FLAGS>(0);
    D3D12_VIDEO_ENCODER_SUPPORT_FLAGS supportFlags =
        static_cast<D3D12_VIDEO_ENCODER_SUPPORT_FLAGS>(0);
    bool videoDeviceAvailable = false;
    bool videoDevice3Available = false;
};

inline std::string HrHex(HRESULT hr) {
    std::ostringstream os;
    os << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return os.str();
}

inline std::string Hex32(uint32_t value) {
    std::ostringstream os;
    os << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return os.str();
}

inline std::string Hex64(uint64_t value) {
    std::ostringstream os;
    os << "0x" << std::hex << std::uppercase << value;
    return os.str();
}

inline std::string LuidHex(LUID luid) {
    std::ostringstream os;
    os << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
       << static_cast<uint32_t>(luid.HighPart)
       << ":0x" << std::setw(8) << static_cast<uint32_t>(luid.LowPart);
    return os.str();
}

inline std::string Narrow(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) {
        return {};
    }

    std::string out(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        out.data(),
        required,
        nullptr,
        nullptr);
    return out;
}

inline std::string ModulePathUtf8(HMODULE module) {
    std::array<wchar_t, 32768> buffer = {};
    const DWORD len = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0 || len >= buffer.size()) {
        return {};
    }
    return Narrow(std::wstring(buffer.data(), len));
}

template <typename T>
inline std::string ByteDump(const T& value) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(&value);
    std::ostringstream os;
    os << std::hex << std::uppercase << std::setfill('0');
    for (size_t i = 0; i < sizeof(T); ++i) {
        if (i != 0) {
            os << ' ';
        }
        os << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return os.str();
}

inline const char* FeatureLevelName(D3D_FEATURE_LEVEL level) noexcept {
    switch (level) {
    case D3D_FEATURE_LEVEL_12_2: return "12_2";
    case D3D_FEATURE_LEVEL_12_1: return "12_1";
    case D3D_FEATURE_LEVEL_12_0: return "12_0";
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    default: return "unknown";
    }
}

inline D3D_FEATURE_LEVEL QueryFeatureLevel(ID3D12Device* device, HRESULT& hr) {
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_12_2,
        D3D_FEATURE_LEVEL_12_1,
        D3D_FEATURE_LEVEL_12_0,
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
    };

    D3D12_FEATURE_DATA_FEATURE_LEVELS data = {};
    data.NumFeatureLevels = static_cast<UINT>(std::size(levels));
    data.pFeatureLevelsRequested = levels;

    hr = device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &data, sizeof(data));
    return SUCCEEDED(hr) ? data.MaxSupportedFeatureLevel : D3D_FEATURE_LEVEL_11_0;
}

inline void PrintDefinition(std::ostream& os, const char* name, bool isDefined) {
    os << "    " << name << "=" << (isDefined ? "defined" : "not-defined") << "\n";
}

inline void PrintBuildProvenance(D3D12CoreLib::D3D12Core& core, std::ostream& os) {
    DXGI_ADAPTER_DESC1 adapterDesc = {};
    HRESULT adapterDescHr = E_FAIL;
    if (auto* adapter = core.DeviceContext().GetAdapter()) {
        adapterDescHr = adapter->GetDesc1(&adapterDesc);
    }

    os << "Binary provenance\n";
    os << "  executable absolute path: " << ModulePathUtf8(nullptr) << "\n";
    os << "  D3DVideoEncoder library: static library linked into executable\n";
    os << "  D3D12Helper library/module: static library linked into executable\n";
#ifdef NDEBUG
    os << "  build configuration: Release (NDEBUG defined)\n";
#else
    os << "  build configuration: Debug (NDEBUG not defined)\n";
#endif
    os << "  Windows SDK target version: " << D3DVE_DIAGNOSTIC_WINDOWS_SDK_VERSION << "\n";
    os << "  relevant compile definitions:\n";
#ifdef D3DVIDEOENCODER_HAS_D3D12_VIDEO_ENCODE
    PrintDefinition(os, "D3DVIDEOENCODER_HAS_D3D12_VIDEO_ENCODE", true);
#else
    PrintDefinition(os, "D3DVIDEOENCODER_HAS_D3D12_VIDEO_ENCODE", false);
#endif
#ifdef D3DVIDEOENCODER_TEST_HAS_D3D12_VIDEO_ENCODE
    PrintDefinition(os, "D3DVIDEOENCODER_TEST_HAS_D3D12_VIDEO_ENCODE", true);
#else
    PrintDefinition(os, "D3DVIDEOENCODER_TEST_HAS_D3D12_VIDEO_ENCODE", false);
#endif
#ifdef _DEBUG
    PrintDefinition(os, "_DEBUG", true);
#else
    PrintDefinition(os, "_DEBUG", false);
#endif
#ifdef NDEBUG
    PrintDefinition(os, "NDEBUG", true);
#else
    PrintDefinition(os, "NDEBUG", false);
#endif
#ifdef WIN32_LEAN_AND_MEAN
    PrintDefinition(os, "WIN32_LEAN_AND_MEAN", true);
#else
    PrintDefinition(os, "WIN32_LEAN_AND_MEAN", false);
#endif
#ifdef NOMINMAX
    PrintDefinition(os, "NOMINMAX", true);
#else
    PrintDefinition(os, "NOMINMAX", false);
#endif
#ifdef _MSC_VER
    os << "    _MSC_VER=" << _MSC_VER << "\n";
#endif
#ifdef _MSC_FULL_VER
    os << "    _MSC_FULL_VER=" << _MSC_FULL_VER << "\n";
#endif
    os << "  adapter desc hr: " << HrHex(adapterDescHr) << "\n";
    os << "  adapter vendor ID: " << Hex32(adapterDesc.VendorId)
       << " device ID: " << Hex32(adapterDesc.DeviceId) << "\n";
}

inline void PrintLightweightProfileLevelProbe(
    ID3D12VideoDevice* videoDevice,
    std::ostream& os,
    NativeD3D12H264Nv12DiagnosticResult& result) {

    D3D12_VIDEO_ENCODER_PROFILE_H264 h264Profile = D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH;
    D3D12_VIDEO_ENCODER_LEVELS_H264 minLevel = D3D12_VIDEO_ENCODER_LEVELS_H264_1;
    D3D12_VIDEO_ENCODER_LEVELS_H264 maxLevel = D3D12_VIDEO_ENCODER_LEVELS_H264_52;

    D3D12_VIDEO_ENCODER_PROFILE_DESC profile = {};
    profile.DataSize = sizeof(h264Profile);
    profile.pH264Profile = &h264Profile;

    D3D12_VIDEO_ENCODER_LEVEL_SETTING minLevelDesc = {};
    minLevelDesc.DataSize = sizeof(minLevel);
    minLevelDesc.pH264LevelSetting = &minLevel;

    D3D12_VIDEO_ENCODER_LEVEL_SETTING maxLevelDesc = {};
    maxLevelDesc.DataSize = sizeof(maxLevel);
    maxLevelDesc.pH264LevelSetting = &maxLevel;

    D3D12_FEATURE_DATA_VIDEO_ENCODER_PROFILE_LEVEL profileLevel = {};
    profileLevel.NodeIndex = 0;
    profileLevel.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
    profileLevel.Profile = profile;
    profileLevel.MinSupportedLevel = minLevelDesc;
    profileLevel.MaxSupportedLevel = maxLevelDesc;

    const auto featureBytesBefore = ByteDump(profileLevel);
    const auto profileBytesBefore = ByteDump(h264Profile);
    const auto minLevelBytesBefore = ByteDump(minLevel);
    const auto maxLevelBytesBefore = ByteDump(maxLevel);
    const BOOL isSupportedBefore = profileLevel.IsSupported;

    result.profileLevelHr = videoDevice
        ? videoDevice->CheckFeatureSupport(
              D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL,
              &profileLevel,
              sizeof(profileLevel))
        : E_POINTER;
    result.profileLevelIsSupported = profileLevel.IsSupported;

    os << "Lightweight profile-level exact query\n";
    os << "  feature enum: D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL ("
       << static_cast<unsigned>(D3D12_FEATURE_VIDEO_ENCODER_PROFILE_LEVEL) << ")\n";
    os << "  feature data sizeof: " << sizeof(profileLevel) << "\n";
    os << "  NodeIndex: " << profileLevel.NodeIndex << "\n";
    os << "  codec enum numeric value: " << static_cast<unsigned>(profileLevel.Codec)
       << " hex=" << Hex32(static_cast<uint32_t>(profileLevel.Codec)) << "\n";
    os << "  profile structure size: " << profile.DataSize << "\n";
    os << "  profile enum numeric value before/after: "
       << static_cast<unsigned>(D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH) << "/"
       << static_cast<unsigned>(h264Profile)
       << " hex=" << Hex32(static_cast<uint32_t>(h264Profile)) << "\n";
    os << "  input format numeric value: " << static_cast<unsigned>(DXGI_FORMAT_NV12)
       << " hex=" << Hex32(static_cast<uint32_t>(DXGI_FORMAT_NV12)) << "\n";
    os << "  resolution: 640x360\n";
    os << "  CheckFeatureSupport HRESULT: " << HrHex(result.profileLevelHr) << "\n";
    os << "  IsSupported before/after: " << isSupportedBefore << "/"
       << profileLevel.IsSupported << "\n";
    os << "  MinSupportedLevel value before/after: "
       << static_cast<unsigned>(D3D12_VIDEO_ENCODER_LEVELS_H264_1) << "/"
       << static_cast<unsigned>(minLevel) << "\n";
    os << "  MaxSupportedLevel value before/after: "
       << static_cast<unsigned>(D3D12_VIDEO_ENCODER_LEVELS_H264_52) << "/"
       << static_cast<unsigned>(maxLevel) << "\n";
    os << "  feature bytes before: " << featureBytesBefore << "\n";
    os << "  feature bytes after:  " << ByteDump(profileLevel) << "\n";
    os << "  profile pointee bytes before: " << profileBytesBefore << "\n";
    os << "  profile pointee bytes after:  " << ByteDump(h264Profile) << "\n";
    os << "  min level pointee bytes before: " << minLevelBytesBefore << "\n";
    os << "  min level pointee bytes after:  " << ByteDump(minLevel) << "\n";
    os << "  max level pointee bytes before: " << maxLevelBytesBefore << "\n";
    os << "  max level pointee bytes after:  " << ByteDump(maxLevel) << "\n";
}

inline HRESULT RunFullSupportQuery(
    ID3D12VideoDevice* videoDevice,
    bool provideSuggestedOutputBuffers,
    std::ostream& os,
    D3D12_VIDEO_ENCODER_VALIDATION_FLAGS& validationFlags,
    D3D12_VIDEO_ENCODER_SUPPORT_FLAGS& supportFlags) {

    constexpr uint32_t kWidth = 640;
    constexpr uint32_t kHeight = 360;
    constexpr uint32_t kBitrate = 4'000'000;
    constexpr uint32_t kFrameRateNum = 30;
    constexpr uint32_t kFrameRateDen = 1;
    constexpr uint32_t kGopLength = 15;

    D3D12_VIDEO_ENCODER_PROFILE_H264 h264Profile = D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH;
    D3D12_VIDEO_ENCODER_PROFILE_DESC profile = {};
    profile.DataSize = sizeof(h264Profile);
    profile.pH264Profile = &h264Profile;

    D3D12_VIDEO_ENCODER_PROFILE_H264 suggestedH264Profile = D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH;
    D3D12_VIDEO_ENCODER_PROFILE_DESC suggestedProfile = {};
    if (provideSuggestedOutputBuffers) {
        suggestedProfile.DataSize = sizeof(suggestedH264Profile);
        suggestedProfile.pH264Profile = &suggestedH264Profile;
    }

    D3D12_VIDEO_ENCODER_LEVELS_H264 suggestedH264Level = D3D12_VIDEO_ENCODER_LEVELS_H264_52;
    D3D12_VIDEO_ENCODER_LEVEL_SETTING suggestedLevel = {};
    if (provideSuggestedOutputBuffers) {
        suggestedLevel.DataSize = sizeof(suggestedH264Level);
        suggestedLevel.pH264LevelSetting = &suggestedH264Level;
    }

    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 h264Config = {};
    h264Config.ConfigurationFlags = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_NONE;
    h264Config.DirectModeConfig = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES_DISABLED;
    h264Config.DisableDeblockingFilterConfig =
        D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_SLICES_DEBLOCKING_MODE_0_ALL_LUMA_CHROMA_SLICE_BLOCK_EDGES_ALWAYS_FILTERED;
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION codecConfig = {};
    codecConfig.DataSize = sizeof(h264Config);
    codecConfig.pH264Config = &h264Config;

    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264 h264Gop = {};
    h264Gop.GOPLength = kGopLength;
    h264Gop.PPicturePeriod = 1;
    h264Gop.pic_order_cnt_type = 0;
    h264Gop.log2_max_frame_num_minus4 = 4;
    h264Gop.log2_max_pic_order_cnt_lsb_minus4 = 4;
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE gop = {};
    gop.DataSize = sizeof(h264Gop);
    gop.pH264GroupOfPictures = &h264Gop;

    D3D12_VIDEO_ENCODER_RATE_CONTROL_CBR cbr = {};
    cbr.InitialQP = 26;
    cbr.MinQP = 0;
    cbr.MaxQP = 51;
    cbr.MaxFrameBitSize = 0;
    cbr.TargetBitRate = kBitrate;
    cbr.VBVCapacity = kBitrate;
    cbr.InitialVBVFullness = kBitrate / 2;
    D3D12_VIDEO_ENCODER_RATE_CONTROL rateControl = {};
    rateControl.Flags = D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_NONE;
    rateControl.TargetFrameRate.Numerator = kFrameRateNum;
    rateControl.TargetFrameRate.Denominator = kFrameRateDen;
    rateControl.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR;
    rateControl.ConfigParams.DataSize = sizeof(cbr);
    rateControl.ConfigParams.pConfiguration_CBR = &cbr;

    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC resolution = { kWidth, kHeight };
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS resolutionLimits = {};

    D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT support = {};
    support.NodeIndex = 0;
    support.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
    support.InputFormat = DXGI_FORMAT_NV12;
    support.CodecConfiguration = codecConfig;
    support.CodecGopSequence = gop;
    support.RateControl = rateControl;
    support.IntraRefresh = D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE;
    support.SubregionFrameEncoding = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
    support.ResolutionsListCount = 1;
    support.pResolutionList = &resolution;
    support.MaxReferenceFramesInDPB = 1;
    support.SuggestedProfile = suggestedProfile;
    support.SuggestedLevel = suggestedLevel;
    support.pResolutionDependentSupport = &resolutionLimits;

    const auto supportBytesBefore = ByteDump(support);
    const auto profileBytesBefore = ByteDump(h264Profile);
    const auto configBytesBefore = ByteDump(h264Config);
    const auto gopBytesBefore = ByteDump(h264Gop);
    const auto rateControlBytesBefore = ByteDump(cbr);
    const auto resolutionBytesBefore = ByteDump(resolution);
    const auto resolutionLimitsBytesBefore = ByteDump(resolutionLimits);
    const auto suggestedProfileBytesBefore = ByteDump(suggestedH264Profile);
    const auto suggestedLevelBytesBefore = ByteDump(suggestedH264Level);

    const HRESULT hr = videoDevice
        ? videoDevice->CheckFeatureSupport(
              D3D12_FEATURE_VIDEO_ENCODER_SUPPORT,
              &support,
              sizeof(support))
        : E_POINTER;

    validationFlags = support.ValidationFlags;
    supportFlags = support.SupportFlags;

    os << "Full support query "
       << (provideSuggestedOutputBuffers ? "(with suggested output buffers)" : "(legacy unbuffered, no suggested output buffers)")
       << "\n";
    os << "  feature enum: D3D12_FEATURE_VIDEO_ENCODER_SUPPORT ("
       << static_cast<unsigned>(D3D12_FEATURE_VIDEO_ENCODER_SUPPORT) << ")\n";
    os << "  sizeof(D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT): " << sizeof(support) << "\n";
    os << "  NodeIndex: " << support.NodeIndex << "\n";
    os << "  Codec: " << static_cast<unsigned>(support.Codec)
       << " hex=" << Hex32(static_cast<uint32_t>(support.Codec)) << "\n";
    os << "  InputFormat: " << static_cast<unsigned>(support.InputFormat)
       << " hex=" << Hex32(static_cast<uint32_t>(support.InputFormat)) << "\n";
    os << "  CodecConfiguration.DataSize: " << codecConfig.DataSize
       << " h264.ConfigurationFlags=" << Hex32(static_cast<uint32_t>(h264Config.ConfigurationFlags))
       << " DirectMode=" << static_cast<unsigned>(h264Config.DirectModeConfig)
       << " Deblock=" << static_cast<unsigned>(h264Config.DisableDeblockingFilterConfig) << "\n";
    os << "  CodecGopSequence.DataSize: " << gop.DataSize
       << " GOPLength=" << h264Gop.GOPLength
       << " PPicturePeriod=" << h264Gop.PPicturePeriod
       << " pic_order_cnt_type=" << static_cast<unsigned>(h264Gop.pic_order_cnt_type)
       << " log2_max_frame_num_minus4=" << static_cast<unsigned>(h264Gop.log2_max_frame_num_minus4)
       << " log2_max_pic_order_cnt_lsb_minus4=" << static_cast<unsigned>(h264Gop.log2_max_pic_order_cnt_lsb_minus4) << "\n";
    os << "  RateControl.Flags: " << Hex32(static_cast<uint32_t>(rateControl.Flags))
       << " Mode=" << static_cast<unsigned>(rateControl.Mode)
       << " TargetFrameRate=" << rateControl.TargetFrameRate.Numerator << "/"
       << rateControl.TargetFrameRate.Denominator
       << " ConfigDataSize=" << rateControl.ConfigParams.DataSize
       << " TargetBitRate=" << cbr.TargetBitRate
       << " VBVCapacity=" << cbr.VBVCapacity
       << " InitialVBVFullness=" << cbr.InitialVBVFullness << "\n";
    os << "  IntraRefresh: " << static_cast<unsigned>(support.IntraRefresh) << "\n";
    os << "  SubregionFrameEncoding: " << static_cast<unsigned>(support.SubregionFrameEncoding) << "\n";
    os << "  ResolutionsListCount: " << support.ResolutionsListCount
       << " pResolutionList=set Width=" << resolution.Width
       << " Height=" << resolution.Height << "\n";
    os << "  MaxReferenceFramesInDPB: " << support.MaxReferenceFramesInDPB << "\n";
    os << "  ValidationFlags before/after: 0x0/" << Hex64(static_cast<uint64_t>(support.ValidationFlags)) << "\n";
    os << "  SupportFlags before/after: 0x0/" << Hex64(static_cast<uint64_t>(support.SupportFlags)) << "\n";
    os << "  SuggestedProfile.DataSize: " << support.SuggestedProfile.DataSize
       << " pointer=" << (support.SuggestedProfile.pH264Profile ? "set" : "null")
       << " value=" << static_cast<unsigned>(suggestedH264Profile) << "\n";
    os << "  SuggestedLevel.DataSize: " << support.SuggestedLevel.DataSize
       << " pointer=" << (support.SuggestedLevel.pH264LevelSetting ? "set" : "null")
       << " value=" << static_cast<unsigned>(suggestedH264Level) << "\n";
    os << "  ResolutionDependentSupport pointer=set MaxSubregionsNumber="
       << resolutionLimits.MaxSubregionsNumber
       << " MaxIntraRefreshFrameDuration=" << resolutionLimits.MaxIntraRefreshFrameDuration
       << " SubregionBlockPixelsSize=" << resolutionLimits.SubregionBlockPixelsSize
       << " QPMapRegionPixelsSize=" << resolutionLimits.QPMapRegionPixelsSize << "\n";
    os << "  CheckFeatureSupport HRESULT: " << HrHex(hr) << "\n";
    os << "  support bytes before: " << supportBytesBefore << "\n";
    os << "  support bytes after:  " << ByteDump(support) << "\n";
    os << "  profile pointee bytes before: " << profileBytesBefore << "\n";
    os << "  profile pointee bytes after:  " << ByteDump(h264Profile) << "\n";
    os << "  config pointee bytes before: " << configBytesBefore << "\n";
    os << "  config pointee bytes after:  " << ByteDump(h264Config) << "\n";
    os << "  gop pointee bytes before: " << gopBytesBefore << "\n";
    os << "  gop pointee bytes after:  " << ByteDump(h264Gop) << "\n";
    os << "  rate-control pointee bytes before: " << rateControlBytesBefore << "\n";
    os << "  rate-control pointee bytes after:  " << ByteDump(cbr) << "\n";
    os << "  resolution pointee bytes before: " << resolutionBytesBefore << "\n";
    os << "  resolution pointee bytes after:  " << ByteDump(resolution) << "\n";
    os << "  suggested profile pointee bytes before: " << suggestedProfileBytesBefore << "\n";
    os << "  suggested profile pointee bytes after:  " << ByteDump(suggestedH264Profile) << "\n";
    os << "  suggested level pointee bytes before: " << suggestedLevelBytesBefore << "\n";
    os << "  suggested level pointee bytes after:  " << ByteDump(suggestedH264Level) << "\n";
    os << "  resolution support pointee bytes before: " << resolutionLimitsBytesBefore << "\n";
    os << "  resolution support pointee bytes after:  " << ByteDump(resolutionLimits) << "\n";

    return hr;
}

inline NativeD3D12H264Nv12DiagnosticResult PrintNativeD3D12H264Nv12Diagnostic(
    D3D12CoreLib::D3D12Core& core,
    std::ostream& os) {

    constexpr uint32_t kWidth = 640;
    constexpr uint32_t kHeight = 360;

    const auto adapterName = Narrow(core.DeviceContext().GetAdapterName());
    const LUID luid = core.GetAdapterLuid();
    constexpr uint32_t kExpectedLuidHigh = 0x00000000u;
    constexpr uint32_t kExpectedLuidLow = 0x000104D6u;

    HRESULT featureLevelHr = S_OK;
    const D3D_FEATURE_LEVEL featureLevel = QueryFeatureLevel(core.GetDevice(), featureLevelHr);

    NativeD3D12H264Nv12DiagnosticResult result;
    result.lightweight = D3D12VideoEncoder::QueryD3D12VideoEncodeSupport(
        &core,
        VideoCodec::H264,
        VideoPixelFormat::NV12,
        kWidth,
        kHeight);

    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D12VideoDevice> videoDevice;
    HRESULT videoDeviceHr = core.GetDevice()->QueryInterface(IID_PPV_ARGS(&videoDevice));
    result.videoDeviceAvailable = SUCCEEDED(videoDeviceHr) && videoDevice;

    ComPtr<ID3D12VideoDevice3> videoDevice3;
    HRESULT videoDevice3Hr = core.GetDevice()->QueryInterface(IID_PPV_ARGS(&videoDevice3));
    result.videoDevice3Available = SUCCEEDED(videoDevice3Hr) && videoDevice3;

    const bool expectedAdapter =
        adapterName == "NVIDIA GeForce RTX 5070 Ti" &&
        static_cast<uint32_t>(luid.HighPart) == kExpectedLuidHigh &&
        static_cast<uint32_t>(luid.LowPart) == kExpectedLuidLow;

    os << "Unified native D3D12 Video Encode diagnostic\n";
    PrintBuildProvenance(core, os);
    os << "Device and adapter\n";
    os << "  device/adapter name: " << adapterName << "\n";
    os << "  adapter LUID: " << LuidHex(luid) << "\n";
    os << "  expected adapter: NVIDIA GeForce RTX 5070 Ti\n";
    os << "  expected LUID: 0x00000000:0x000104D6\n";
    os << "  expected adapter match: " << (expectedAdapter ? "yes" : "no") << "\n";
    os << "  feature level: " << FeatureLevelName(featureLevel)
       << " hr=" << HrHex(featureLevelHr) << "\n";
    os << "  ID3D12VideoDevice: " << (result.videoDeviceAvailable ? "yes" : "no")
       << " hr=" << HrHex(videoDeviceHr) << "\n";
    os << "  ID3D12VideoDevice3: " << (result.videoDevice3Available ? "yes" : "no")
       << " hr=" << HrHex(videoDevice3Hr) << "\n";
    os << "  allowWarpAdapter: false\n";
    os << "  NodeIndex: 0\n";
    os << "  codec: H.264 (" << static_cast<unsigned>(D3D12_VIDEO_ENCODER_CODEC_H264) << ")\n";
    os << "  profile: H264_HIGH (" << static_cast<unsigned>(D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH) << ")\n";
    os << "  input format: NV12 (" << static_cast<unsigned>(DXGI_FORMAT_NV12) << ")\n";
    os << "  resolution: 640x360\n";
    os << "  subregion mode: FULL_FRAME ("
       << static_cast<unsigned>(D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME) << ")\n";
    os << "  rate-control mode: CBR ("
       << static_cast<unsigned>(D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR) << ")\n";
    os << "  lightweight public query result: supported=" << result.lightweight.supported
       << " hr=" << HrHex(result.lightweight.queryHr)
       << " message=" << result.lightweight.message << "\n";

    PrintLightweightProfileLevelProbe(videoDevice.Get(), os, result);
    result.fullQueryUnbufferedHr = RunFullSupportQuery(
        videoDevice.Get(),
        false,
        os,
        result.validationFlags,
        result.supportFlags);
    result.fullQueryBufferedHr = RunFullSupportQuery(
        videoDevice.Get(),
        true,
        os,
        result.validationFlags,
        result.supportFlags);

    os << "Unified result summary\n";
    os << "  lightweight public query: supported=" << result.lightweight.supported
       << " hr=" << HrHex(result.lightweight.queryHr) << "\n";
    os << "  lightweight profile-level query: supported=" << result.profileLevelIsSupported
       << " hr=" << HrHex(result.profileLevelHr) << "\n";
    os << "  full query legacy unbuffered: hr=" << HrHex(result.fullQueryUnbufferedHr) << "\n";
    os << "  full query buffered: hr=" << HrHex(result.fullQueryBufferedHr)
       << " supportFlags=" << Hex64(static_cast<uint64_t>(result.supportFlags))
       << " validationFlags=" << Hex64(static_cast<uint64_t>(result.validationFlags))
       << " generalSupportOk="
       << ((result.supportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK) ? "yes" : "no")
       << "\n";

    return result;
}

} // namespace D3DVideoEncoderLib::Diagnostics
