#pragma once

#include <cstdint>

namespace D3DVideoEncoderLib {

class TimestampGenerator {
public:
    TimestampGenerator() = default;
    TimestampGenerator(uint32_t frameRateNum, uint32_t frameRateDen);

    int64_t nextTimestamp100ns();
    int64_t timestampForFrame100ns(uint64_t frameIndex) const;
    int64_t frameDuration100ns() const noexcept { return duration100ns_; }
    void reset() noexcept { frameIndex_ = 0; }

private:
    uint32_t frameRateNum_ = 0;
    uint32_t frameRateDen_ = 0;
    int64_t duration100ns_ = 0;
    uint64_t frameIndex_ = 0;
};

} // namespace D3DVideoEncoderLib
