#include "backend/mux/NvencMp4DurationBoxes.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace D3DVideoEncoderLib;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

uint64_t read_u64(const std::vector<uint8_t>& bytes, size_t offset) {
    require(offset + 8 <= bytes.size(), "duration offset is outside box");
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) value = (value << 8) | bytes[offset + i];
    return value;
}

void check_box(const std::vector<uint8_t>& box, uint64_t duration, size_t durationOffset) {
    require(box.size() > durationOffset + 7, "duration box is too small");
    require(box[8] == 1, "mvhd/tkhd/mdhd must use version 1");
    require(read_u64(box, durationOffset) == duration, "64-bit MP4 duration mismatch");
}

void check_duration(uint64_t duration) {
    check_box(NvencMuxerInternal::MakeMvhd(duration), duration, 32);
    check_box(NvencMuxerInternal::MakeTkhd(640, 360, duration), duration, 36);
    check_box(NvencMuxerInternal::MakeMdhd(duration), duration, 32);
}

} // namespace

int main() {
    check_duration(0xffffffffull - 1ull);
    check_duration(0xffffffffull + 10'000'000ull);
    std::cout << "NVENC MP4 duration tests passed.\n";
    return 0;
}
