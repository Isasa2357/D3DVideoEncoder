#pragma once

#include <nvEncodeAPI.h>

#include <cstdint>

namespace D3DVideoEncoderLib {

inline bool ShouldDrainNvencEos(NVENCSTATUS status, uint64_t sentFrameCount, uint64_t receivedPacketCount) noexcept {
    return status == NV_ENC_SUCCESS && receivedPacketCount < sentFrameCount;
}

inline bool IsSupportedNvencBFrameCount(uint32_t bFrameCount) noexcept {
    return bFrameCount == 0;
}

} // namespace D3DVideoEncoderLib
