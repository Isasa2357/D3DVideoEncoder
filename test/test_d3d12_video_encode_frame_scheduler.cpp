#include "backend/d3d12video/D3D12VideoEncodeFrameScheduler.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <vector>

using namespace D3DVideoEncoderLib;

namespace {

const char* frame_type_name(D3D12VideoEncodeFrameType t) {
    switch (t) {
    case D3D12VideoEncodeFrameType::IDR: return "IDR";
    case D3D12VideoEncodeFrameType::P: return "P";
    case D3D12VideoEncodeFrameType::B: return "B";
    default: return "?";
    }
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        std::exit(1);
    }
}

void require_type(const D3D12VideoEncodeFramePlan& p, D3D12VideoEncodeFrameType expected, const char* message) {
    if (p.frameType != expected) {
        std::cerr << "FAILED: " << message << " expected=" << frame_type_name(expected)
                  << " actual=" << frame_type_name(p.frameType) << "\n";
        std::exit(1);
    }
}

void test_p_only_schedule() {
    D3D12VideoEncodeFrameScheduler s({4, 0, true});
    const auto plan = s.makePlan(10);
    require(plan.size() == 10, "P-only plan should contain all frames");

    for (size_t i = 0; i < plan.size(); ++i) {
        require(plan[i].encodeOrderIndex == i, "P-only encode order must equal display order");
        require(plan[i].displayOrderIndex == i, "P-only display order must be sequential");
        if ((i % 4) == 0) {
            require_type(plan[i], D3D12VideoEncodeFrameType::IDR, "GOP start must be IDR");
            require(plan[i].list0DisplayOrder == -1, "IDR must not use list0 reference");
        } else {
            require_type(plan[i], D3D12VideoEncodeFrameType::P, "Non-GOP frame must be P in P-only mode");
            require(plan[i].list0DisplayOrder == static_cast<int64_t>(i - 1), "P frame should refer to previous display frame in P-only mode");
        }
        require(plan[i].usedAsReference, "IDR/P frames must be reference frames");
    }
}

void test_b_frame_schedule_order() {
    D3D12VideoEncodeFrameScheduler s({8, 2, true});
    const auto plan = s.makePlan(8);
    require(plan.size() == 8, "B-frame plan should contain all frames");

    const std::vector<uint64_t> expectedDisplayOrder = {0, 3, 1, 2, 6, 4, 5, 7};
    const std::vector<D3D12VideoEncodeFrameType> expectedTypes = {
        D3D12VideoEncodeFrameType::IDR,
        D3D12VideoEncodeFrameType::P,
        D3D12VideoEncodeFrameType::B,
        D3D12VideoEncodeFrameType::B,
        D3D12VideoEncodeFrameType::P,
        D3D12VideoEncodeFrameType::B,
        D3D12VideoEncodeFrameType::B,
        D3D12VideoEncodeFrameType::P,
    };

    std::set<uint64_t> seen;
    for (size_t i = 0; i < plan.size(); ++i) {
        require(plan[i].encodeOrderIndex == i, "encodeOrderIndex must be sequential");
        require(plan[i].displayOrderIndex == expectedDisplayOrder[i], "B-frame encode/display order mismatch");
        require_type(plan[i], expectedTypes[i], "B-frame type mismatch");
        require(seen.insert(plan[i].displayOrderIndex).second, "display frame emitted more than once");

        if (plan[i].frameType == D3D12VideoEncodeFrameType::B) {
            require(!plan[i].usedAsReference, "B frame must not be kept as reference in initial scheduler");
            require(plan[i].list0DisplayOrder >= 0, "B frame requires list0 reference");
            require(plan[i].list1DisplayOrder >= 0, "B frame requires list1 reference");
            require(plan[i].list0DisplayOrder < static_cast<int64_t>(plan[i].displayOrderIndex), "B list0 should be previous reference");
            require(plan[i].list1DisplayOrder > static_cast<int64_t>(plan[i].displayOrderIndex), "B list1 should be future reference");
        }
    }
}

void test_closed_gop_does_not_cross_next_idr() {
    D3D12VideoEncodeFrameScheduler s({5, 2, true});
    const auto plan = s.makePlan(12);
    require(!plan.empty(), "closed GOP plan should not be empty");

    for (const auto& p : plan) {
        const uint64_t gopStart = (p.displayOrderIndex / 5) * 5;
        const uint64_t gopEnd = gopStart + 5;
        if (p.list0DisplayOrder >= 0) {
            require(static_cast<uint64_t>(p.list0DisplayOrder) >= gopStart, "list0 reference crossed previous GOP");
            require(static_cast<uint64_t>(p.list0DisplayOrder) < gopEnd, "list0 reference crossed next GOP");
        }
        if (p.list1DisplayOrder >= 0) {
            require(static_cast<uint64_t>(p.list1DisplayOrder) >= gopStart, "list1 reference crossed previous GOP");
            require(static_cast<uint64_t>(p.list1DisplayOrder) < gopEnd, "list1 reference crossed next GOP");
        }
    }
}

void test_validation() {
    bool threw = false;
    try {
        D3D12VideoEncodeFrameScheduler s({0, 0, true});
        (void)s;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "gopLength=0 must be rejected");

    threw = false;
    try {
        D3D12VideoEncodeFrameScheduler s({30, 17, true});
        (void)s;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "excessive bFrameCount must be rejected");
}

} // namespace

int main() {
    test_p_only_schedule();
    test_b_frame_schedule_order();
    test_closed_gop_does_not_cross_next_idr();
    test_validation();
    std::cout << "D3D12 Video Encode frame scheduler tests passed.\n";
    return 0;
}
