#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

#include <exception>
#include <iostream>

using namespace D3DVideoEncoderLib;

namespace {
void print_cap(const char* label, const D3D12VideoEncodeFormatCapability& cap) {
    std::cout << label
              << " videoDevice=" << cap.videoDeviceAvailable
              << " videoDevice3=" << cap.videoDevice3Available
              << " featureArea=" << cap.featureAreaSupported
              << " codec=" << cap.codecSupported
              << " profile=" << cap.profileSupported
              << " format=" << cap.inputFormatSupported
              << " rc=" << cap.rateControlSupported
              << " cbr=" << cap.cbrSupported
              << " cqp=" << cap.cqpSupported
              << " resolution=" << cap.outputResolutionSupported
              << " heap=" << cap.heapSizeSupported
              << " supported=" << cap.supported
              << " min=" << cap.minWidth << "x" << cap.minHeight
              << " max=" << cap.maxWidth << "x" << cap.maxHeight
              << " multiple=" << cap.widthMultiple << "x" << cap.heightMultiple
              << " heapL0=" << cap.heapMemoryPoolL0Size
              << " heapL1=" << cap.heapMemoryPoolL1Size
              << " message=" << cap.message << "\n";
}
}

int main() {
    try {
        D3D12CoreLib::D3D12CoreConfig cfg;
        cfg.enableDebugLayer = false;
        cfg.enableInfoQueue = false;
        cfg.enableDred = false;
        cfg.allowWarpAdapter = true;
        auto core = D3D12CoreLib::D3D12Core::CreateShared(cfg);

        constexpr uint32_t width = 640;
        constexpr uint32_t height = 360;
        const auto caps = D3D12VideoEncoder::QueryD3D12VideoEncodeCapabilities(core.get(), width, height);

        print_cap("D3D12VideoEncode H264/NV12", caps.h264Nv12);
        print_cap("D3D12VideoEncode HEVC/NV12", caps.hevcNv12);
        print_cap("D3D12VideoEncode HEVC/P010", caps.hevcP010);

        if (!caps.supportsH264Nv12()) {
            std::cout << "D3D12VideoEncode H.264/NV12 is not supported on this device/driver. Treating as skipped.\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "D3D12 Video Encode capability query failed unexpectedly: " << e.what() << "\n";
        return 1;
    }
}
