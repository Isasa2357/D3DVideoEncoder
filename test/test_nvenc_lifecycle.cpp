#include "backend/nvenc/NvencCommon.hpp"
#include "backend/nvenc/NvencLifecyclePolicy.hpp"

#include <iostream>
#include <stdexcept>

using namespace D3DVideoEncoderLib;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void require_b_frames_rejected(NvencDirectXDeviceKind deviceKind) {
    NvencSessionDesc desc = {};
    desc.outputPath = L"unused.h264";
    desc.width = 640;
    desc.height = 360;
    desc.codec = VideoCodec::H264;
    desc.inputFormat = VideoPixelFormat::NV12;
    desc.bFrameCount = 1;

    NvencEncoderSession session;
    bool rejected = false;
    try {
        // B-frame validation must happen before this non-null sentinel is ever
        // interpreted as a real DirectX device or the NVENC API is loaded.
        session.initialize(
            reinterpret_cast<void*>(1),
            NV_ENC_DEVICE_TYPE_DIRECTX,
            deviceKind,
            desc);
    } catch (const D3DVideoEncoderError&) {
        rejected = true;
    }
    require(rejected, "NVENC common initialization must reject bFrameCount>0");
}

} // namespace

int main() {
    require(!ShouldDrainNvencEos(NV_ENC_SUCCESS, 30, 30), "EOS must not drain when sent equals received");
    require(ShouldDrainNvencEos(NV_ENC_SUCCESS, 30, 29), "EOS must drain a pending packet");
    require(!ShouldDrainNvencEos(NV_ENC_ERR_NEED_MORE_INPUT, 30, 29), "EOS drain requires NV_ENC_SUCCESS");
    require(IsSupportedNvencBFrameCount(0), "NVENC must accept bFrameCount=0");
    require(!IsSupportedNvencBFrameCount(1), "NVENC must reject unsupported B-frames");
    require_b_frames_rejected(NvencDirectXDeviceKind::D3D11);
    require_b_frames_rejected(NvencDirectXDeviceKind::D3D12);

    NvencEncoderSession unopened;
    unopened.close();
    unopened.close();

    std::cout << "NVENC lifecycle policy tests passed.\n";
    return 0;
}
