#pragma once

#include <nvEncodeAPI.h>

#include <cstdint>

namespace D3DVideoEncoderLib {

inline bool ShouldDrainNvencEos(NVENCSTATUS status, uint64_t sentFrameCount, uint64_t receivedPacketCount) noexcept {
    return status == NV_ENC_SUCCESS && receivedPacketCount < sentFrameCount;
}

} // namespace D3DVideoEncoderLib
