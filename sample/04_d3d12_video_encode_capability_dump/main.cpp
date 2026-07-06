#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <string>

using namespace D3DVideoEncoderLib;

namespace {

const char* yesno(bool v) noexcept { return v ? "yes" : "no"; }

void print_cap(const char* name, const D3D12VideoEncodeFormatCapability& c) {
    std::cout << "[" << name << "]\n";
    std::cout << "  supported:              " << yesno(c.supported) << "\n";
    std::cout << "  videoDeviceAvailable:   " << yesno(c.videoDeviceAvailable) << "\n";
    std::cout << "  videoDevice3Available:  " << yesno(c.videoDevice3Available) << "\n";
    std::cout << "  featureAreaSupported:   " << yesno(c.featureAreaSupported) << "\n";
    std::cout << "  codecSupported:         " << yesno(c.codecSupported) << "\n";
    std::cout << "  profileSupported:       " << yesno(c.profileSupported) << "\n";
    std::cout << "  inputFormatSupported:   " << yesno(c.inputFormatSupported) << "\n";
    std::cout << "  cbrSupported:           " << yesno(c.cbrSupported) << "\n";
    std::cout << "  cqpSupported:           " << yesno(c.cqpSupported) << "\n";
    std::cout << "  outputResolution:       " << yesno(c.outputResolutionSupported) << "\n";
    std::cout << "  heapSizeSupported:      " << yesno(c.heapSizeSupported) << "\n";
    std::cout << "  requested:              " << c.requestedWidth << "x" << c.requestedHeight << "\n";
    std::cout << "  min/max:                " << c.minWidth << "x" << c.minHeight << " - " << c.maxWidth << "x" << c.maxHeight << "\n";
    std::cout << "  multiple:               " << c.widthMultiple << "x" << c.heightMultiple << "\n";
    std::cout << "  message:                " << c.message << "\n\n";
}

uint32_t parse_u32(const wchar_t* s, uint32_t fallback) {
    if (!s) return fallback;
    wchar_t* end = nullptr;
    const unsigned long v = std::wcstoul(s, &end, 10);
    if (end == s || v == 0) return fallback;
    return static_cast<uint32_t>(v);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const uint32_t width = (argc >= 2) ? parse_u32(argv[1], 1920) : 1920;
        const uint32_t height = (argc >= 3) ? parse_u32(argv[2], 1080) : 1080;

        D3D12CoreLib::D3D12CoreConfig cfg;
        cfg.enableDebugLayer = false;
        cfg.enableInfoQueue = false;
        cfg.enableDred = false;
        cfg.allowWarpAdapter = false;
        auto core = D3D12CoreLib::D3D12Core::CreateShared(cfg);

        std::cout << "D3D12 Video Encode capability dump for " << width << "x" << height << "\n\n";
        const auto caps = D3D12VideoEncoder::QueryD3D12VideoEncodeCapabilities(core.get(), width, height);
        print_cap("H.264 / NV12", caps.h264Nv12);
        print_cap("HEVC / NV12", caps.hevcNv12);
        print_cap("HEVC / P010", caps.hevcP010);

        std::cout << "NVENC capability dump for the same D3D12 device.\n";
        const auto nvenc = D3D12VideoEncoder::QueryNvencCapabilities(core.get());
        std::cout << "  H.264/NV12: " << yesno(nvenc.supportsH264Nv12()) << " - " << nvenc.h264Nv12.message << "\n";
        std::cout << "  HEVC/NV12:  " << yesno(nvenc.supportsHevcNv12()) << " - " << nvenc.hevcNv12.message << "\n";
        std::cout << "  HEVC/P010:  " << yesno(nvenc.supportsHevcP010()) << " - " << nvenc.hevcP010.message << "\n";
        std::cout << "  AV1/NV12:   " << yesno(nvenc.supportsAv1Nv12()) << " - " << nvenc.av1Nv12.message << "\n";
        std::cout << "  AV1/P010:   " << yesno(nvenc.supportsAv1P010()) << " - " << nvenc.av1P010.message << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
