#include "util/TimestampGenerator.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>

using namespace D3DVideoEncoderLib;

namespace {
void require_true(bool cond, const char* message) {
    if (!cond) {
        std::cerr << "FAILED: " << message << "\n";
        std::exit(1);
    }
}
}

int main() {
    try {
        TimestampGenerator exact(25, 1);
        require_true(exact.frameDuration100ns() == 400000, "25 fps duration should be exactly 400000 * 100ns");
        require_true(exact.nextTimestamp100ns() == 0, "first timestamp should be zero");
        require_true(exact.nextTimestamp100ns() == 400000, "second timestamp at 25 fps");
        require_true(exact.timestampForFrame100ns(10) == 4000000, "timestampForFrame at 25 fps");
        exact.reset();
        require_true(exact.nextTimestamp100ns() == 0, "reset returns to frame zero");

        TimestampGenerator ntsc(30000, 1001);
        require_true(ntsc.frameDuration100ns() == 333667, "30000/1001 duration should round to 333667 * 100ns");
        require_true(ntsc.nextTimestamp100ns() == 0, "ntsc first timestamp should be zero");
        require_true(ntsc.nextTimestamp100ns() == 333666, "ntsc second timestamp should use rational timestamp floor");
        require_true(ntsc.nextTimestamp100ns() == 667333, "ntsc third timestamp should use rational timestamp floor");

        bool threw = false;
        try {
            TimestampGenerator invalid(0, 1);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require_true(threw, "zero numerator should throw");

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "unexpected exception: " << e.what() << "\n";
        return 1;
    }
}
