#include <D3DVideoEncoder/D3D11VideoEncoder.hpp>
#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

#include <exception>
#include <iostream>

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
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "NVENC capability query failed unexpectedly: " << e.what() << "\n";
        return 1;
    }
}
