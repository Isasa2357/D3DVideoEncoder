#include <D3DVideoEncoder/D3D11VideoEncoder.hpp>
#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

#include "backend/nvenc/NvencCommon.hpp"

#include <algorithm>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

using namespace D3DVideoEncoderLib;

namespace {
void print_cap(const char* label, const NvencFormatCapability& cap) {
    std::cout << label
              << " runtime=" << cap.runtimeAvailable
              << " device=" << cap.deviceSupported
              << " codec=" << cap.codecSupported
              << " format=" << cap.inputFormatSupported
              << " supported=" << cap.supported
              << " max=" << cap.maxWidth << "x" << cap.maxHeight
              << " message=" << cap.message << "\n";
}

bool guid_equal(const GUID& a, const GUID& b) noexcept {
    return IsEqualGUID(a, b) != FALSE;
}

std::string guid_string(const GUID& guid) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(8) << guid.Data1 << "-"
        << std::setw(4) << guid.Data2 << "-"
        << std::setw(4) << guid.Data3 << "-";
    for (int i = 0; i < 2; ++i) oss << std::setw(2) << static_cast<unsigned>(guid.Data4[i]);
    oss << "-";
    for (int i = 2; i < 8; ++i) oss << std::setw(2) << static_cast<unsigned>(guid.Data4[i]);
    return oss.str();
}

const char* known_guid_name(const GUID& guid) {
    if (guid_equal(guid, NV_ENC_CODEC_H264_GUID)) return "H264";
    if (guid_equal(guid, NV_ENC_CODEC_HEVC_GUID)) return "HEVC";
#ifdef NV_ENC_CODEC_AV1_GUID
    if (guid_equal(guid, NV_ENC_CODEC_AV1_GUID)) return "AV1";
#endif
    if (guid_equal(guid, NV_ENC_PRESET_P1_GUID)) return "P1";
    if (guid_equal(guid, NV_ENC_PRESET_P2_GUID)) return "P2";
    if (guid_equal(guid, NV_ENC_PRESET_P3_GUID)) return "P3";
    if (guid_equal(guid, NV_ENC_PRESET_P4_GUID)) return "P4";
    if (guid_equal(guid, NV_ENC_PRESET_P5_GUID)) return "P5";
    if (guid_equal(guid, NV_ENC_PRESET_P6_GUID)) return "P6";
    if (guid_equal(guid, NV_ENC_PRESET_P7_GUID)) return "P7";
    if (guid_equal(guid, NV_ENC_H264_PROFILE_BASELINE_GUID)) return "H264_BASELINE";
    if (guid_equal(guid, NV_ENC_H264_PROFILE_MAIN_GUID)) return "H264_MAIN";
    if (guid_equal(guid, NV_ENC_H264_PROFILE_HIGH_GUID)) return "H264_HIGH";
    return "unknown";
}

void print_guid_list(const char* label, const std::vector<GUID>& guids) {
    std::cout << label << " count=" << guids.size();
    for (const auto& guid : guids) {
        std::cout << " [" << known_guid_name(guid) << ":" << guid_string(guid) << "]";
    }
    std::cout << "\n";
}

int get_cap(NvencApi& api, void* encoder, GUID codec, NV_ENC_CAPS cap) {
    NV_ENC_CAPS_PARAM params = {};
    params.version = NV_ENC_CAPS_PARAM_VER;
    params.capsToQuery = cap;
    int value = 0;
    const NVENCSTATUS status = api.functions().nvEncGetEncodeCaps(encoder, codec, &params, &value);
    if (status != NV_ENC_SUCCESS) {
        std::cout << "cap " << static_cast<int>(cap) << " status=" << NvencStatusToString(status) << "\n";
        return 0;
    }
    return value;
}

void print_nvenc_option_audit(const char* label, void* device, NV_ENC_DEVICE_TYPE deviceType) {
    NvencApi api;
    api.load();

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open = {};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.device = device;
    open.deviceType = deviceType;
    open.apiVersion = NVENCAPI_VERSION;
    void* encoder = nullptr;
    D3DVE_THROW_IF_NVENC_FAILED_MSG(api.functions().nvEncOpenEncodeSessionEx(&open, &encoder), std::string(label) + ": nvEncOpenEncodeSessionEx");

    const GUID codec = NV_ENC_CODEC_H264_GUID;
    uint32_t count = 0;
    D3DVE_THROW_IF_NVENC_FAILED_MSG(api.functions().nvEncGetEncodeGUIDCount(encoder, &count), std::string(label) + ": nvEncGetEncodeGUIDCount");
    std::vector<GUID> encodeGuids(count);
    uint32_t written = 0;
    if (count) {
        D3DVE_THROW_IF_NVENC_FAILED_MSG(api.functions().nvEncGetEncodeGUIDs(encoder, encodeGuids.data(), count, &written), std::string(label) + ": nvEncGetEncodeGUIDs");
        encodeGuids.resize(written);
    }

    count = 0;
    D3DVE_THROW_IF_NVENC_FAILED_MSG(api.functions().nvEncGetEncodePresetCount(encoder, codec, &count), std::string(label) + ": nvEncGetEncodePresetCount");
    std::vector<GUID> presetGuids(count);
    written = 0;
    if (count) {
        D3DVE_THROW_IF_NVENC_FAILED_MSG(api.functions().nvEncGetEncodePresetGUIDs(encoder, codec, presetGuids.data(), count, &written), std::string(label) + ": nvEncGetEncodePresetGUIDs");
        presetGuids.resize(written);
    }

    count = 0;
    D3DVE_THROW_IF_NVENC_FAILED_MSG(api.functions().nvEncGetEncodeProfileGUIDCount(encoder, codec, &count), std::string(label) + ": nvEncGetEncodeProfileGUIDCount");
    std::vector<GUID> profileGuids(count);
    written = 0;
    if (count) {
        D3DVE_THROW_IF_NVENC_FAILED_MSG(api.functions().nvEncGetEncodeProfileGUIDs(encoder, codec, profileGuids.data(), count, &written), std::string(label) + ": nvEncGetEncodeProfileGUIDs");
        profileGuids.resize(written);
    }

    std::cout << label << " option audit\n";
    print_guid_list("  encodeGUIDs", encodeGuids);
    print_guid_list("  presetGUIDs", presetGuids);
    print_guid_list("  profileGUIDs", profileGuids);
    std::cout << "  selected preset P5 supported="
              << (std::any_of(presetGuids.begin(), presetGuids.end(), [](const GUID& g) { return guid_equal(g, NV_ENC_PRESET_P5_GUID); }) ? 1 : 0)
              << " selected profile H264_HIGH supported="
              << (std::any_of(profileGuids.begin(), profileGuids.end(), [](const GUID& g) { return guid_equal(g, NV_ENC_H264_PROFILE_HIGH_GUID); }) ? 1 : 0)
              << "\n";
    std::cout << "  caps rcMask=" << get_cap(api, encoder, codec, NV_ENC_CAPS_SUPPORTED_RATECONTROL_MODES)
              << " async=" << get_cap(api, encoder, codec, NV_ENC_CAPS_ASYNC_ENCODE_SUPPORT)
              << " widthMin=" << get_cap(api, encoder, codec, NV_ENC_CAPS_WIDTH_MIN)
              << " heightMin=" << get_cap(api, encoder, codec, NV_ENC_CAPS_HEIGHT_MIN)
              << " widthMax=" << get_cap(api, encoder, codec, NV_ENC_CAPS_WIDTH_MAX)
              << " heightMax=" << get_cap(api, encoder, codec, NV_ENC_CAPS_HEIGHT_MAX)
              << "\n";

    api.functions().nvEncDestroyEncoder(encoder);
}
}

int main() {
    try {
        D3D11CoreLib::D3D11CoreConfig d3d11Cfg;
        d3d11Cfg.enableDebugLayer = false;
        d3d11Cfg.enableInfoQueue = false;
        d3d11Cfg.allowWarpAdapter = true;
        auto d3d11Core = D3D11CoreLib::D3D11Core::CreateShared(d3d11Cfg);

        D3D12CoreLib::D3D12CoreConfig d3d12Cfg;
        d3d12Cfg.enableDebugLayer = false;
        d3d12Cfg.enableInfoQueue = false;
        d3d12Cfg.enableDred = false;
        d3d12Cfg.allowWarpAdapter = true;
        auto d3d12Core = D3D12CoreLib::D3D12Core::CreateShared(d3d12Cfg);

        const auto d3d11Caps = D3D11VideoEncoder::QueryNvencCapabilities(d3d11Core.get());
        const auto d3d12Caps = D3D12VideoEncoder::QueryNvencCapabilities(d3d12Core.get());
        print_cap("D3D11 H264/NV12", d3d11Caps.h264Nv12);
        print_cap("D3D11 HEVC/P010", d3d11Caps.hevcP010);
        print_cap("D3D12 H264/NV12", d3d12Caps.h264Nv12);
        print_cap("D3D12 HEVC/P010", d3d12Caps.hevcP010);
        print_nvenc_option_audit("D3D11", d3d11Core->GetDevice(), NV_ENC_DEVICE_TYPE_DIRECTX);
        print_nvenc_option_audit("D3D12", d3d12Core->GetDevice(), NV_ENC_DEVICE_TYPE_DIRECTX);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "NVENC capability query failed unexpectedly: " << e.what() << "\n";
        return 1;
    }
}
