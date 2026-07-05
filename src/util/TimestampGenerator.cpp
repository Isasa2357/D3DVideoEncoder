#include "util/TimestampGenerator.hpp"

#include <stdexcept>

namespace D3DVideoEncoderLib {

TimestampGenerator::TimestampGenerator(uint32_t frameRateNum, uint32_t frameRateDen)
    : frameRateNum_(frameRateNum), frameRateDen_(frameRateDen) {
    if (frameRateNum_ == 0 || frameRateDen_ == 0) {
        throw std::invalid_argument("frameRateNum and frameRateDen must be non-zero.");
    }
    duration100ns_ = static_cast<int64_t>((10'000'000ull * frameRateDen_ + frameRateNum_ / 2) / frameRateNum_);
}

int64_t TimestampGenerator::timestampForFrame100ns(uint64_t frameIndex) const {
    return static_cast<int64_t>((frameIndex * 10'000'000ull * frameRateDen_) / frameRateNum_);
}

int64_t TimestampGenerator::nextTimestamp100ns() {
    const int64_t value = timestampForFrame100ns(frameIndex_);
    ++frameIndex_;
    return value;
}

} // namespace D3DVideoEncoderLib
