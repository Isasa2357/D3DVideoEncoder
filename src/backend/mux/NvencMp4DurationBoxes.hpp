#pragma once

#include <cstdint>
#include <vector>

namespace D3DVideoEncoderLib::NvencMuxerInternal {

std::vector<uint8_t> MakeMvhd(uint64_t duration);
std::vector<uint8_t> MakeTkhd(uint32_t width, uint32_t height, uint64_t duration);
std::vector<uint8_t> MakeMdhd(uint64_t duration);

} // namespace D3DVideoEncoderLib::NvencMuxerInternal
