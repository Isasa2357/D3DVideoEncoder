#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace D3DVideoEncoderLib {

// Small, backend-independent planning layer for native D3D12 Video Encode frame order.
//
// The current backend still encodes with bFrameCount=0, but this scheduler makes the
// next B-frame implementation explicit and testable: display order and encode order
// are separated here, while codec-specific picture-control code can consume the
// resulting D3D12VideoEncodeFramePlan later.
enum class D3D12VideoEncodeFrameType {
    IDR,
    P,
    B,
};

struct D3D12VideoEncodeFrameSchedulerDesc {
    // IDR interval in display-order frames. Values smaller than 1 are rejected.
    uint32_t gopLength = 60;

    // Number of B frames between reference pictures. 0 preserves the current
    // IDR/P-only behavior. Values larger than 16 are rejected intentionally; they
    // would require a much larger DPB and are outside the intended low-latency use.
    uint32_t bFrameCount = 0;

    // Closed GOP means B frames never reference an IDR from the next GOP. This is
    // the only mode planned for the first backend integration.
    bool closedGop = true;
};

struct D3D12VideoEncodeFramePlan {
    uint64_t encodeOrderIndex = 0;
    uint64_t displayOrderIndex = 0;
    D3D12VideoEncodeFrameType frameType = D3D12VideoEncodeFrameType::P;

    // True when the reconstructed picture must remain in the DPB for later frames.
    bool usedAsReference = false;

    // Display-order references. -1 means unused.
    int64_t list0DisplayOrder = -1;
    int64_t list1DisplayOrder = -1;

    // Index within the current GOP, counted in display order.
    uint32_t gopDisplayIndex = 0;

    // Consecutive reference-picture index within the current GOP.
    uint32_t referenceIndexInGop = 0;
};

class D3D12VideoEncodeFrameScheduler {
public:
    D3D12VideoEncodeFrameScheduler() = default;
    explicit D3D12VideoEncodeFrameScheduler(D3D12VideoEncodeFrameSchedulerDesc desc) {
        reset(desc);
    }

    void reset(D3D12VideoEncodeFrameSchedulerDesc desc) {
        validate(desc);
        desc_ = desc;
    }

    const D3D12VideoEncodeFrameSchedulerDesc& desc() const noexcept { return desc_; }

    // Generate a finite closed-GOP plan for tests and future backend integration.
    // For bFrameCount=0 this returns display order == encode order.
    // For bFrameCount>0, each GOP is encoded as:
    //   IDR, next-ref, B..., next-ref, B...
    // so B frames can reference the previous and next reference pictures without
    // crossing into the next GOP.
    std::vector<D3D12VideoEncodeFramePlan> makePlan(uint64_t displayFrameCount) const {
        std::vector<D3D12VideoEncodeFramePlan> out;
        out.reserve(static_cast<size_t>(std::min<uint64_t>(displayFrameCount, 4096)));

        uint64_t encodeIndex = 0;
        for (uint64_t gopStart = 0; gopStart < displayFrameCount; gopStart += desc_.gopLength) {
            const uint64_t gopEnd = std::min<uint64_t>(gopStart + desc_.gopLength, displayFrameCount);
            appendGop(out, encodeIndex, gopStart, gopEnd);
        }
        return out;
    }

    static bool isReferenceFrame(D3D12VideoEncodeFrameType type) noexcept {
        return type == D3D12VideoEncodeFrameType::IDR || type == D3D12VideoEncodeFrameType::P;
    }

private:
    static void validate(const D3D12VideoEncodeFrameSchedulerDesc& desc) {
        if (desc.gopLength == 0) {
            throw std::invalid_argument("D3D12VideoEncodeFrameSchedulerDesc.gopLength must be non-zero.");
        }
        if (desc.bFrameCount > 16) {
            throw std::invalid_argument("D3D12VideoEncodeFrameSchedulerDesc.bFrameCount is too large for the initial DPB scheduler.");
        }
        if (!desc.closedGop) {
            throw std::invalid_argument("D3D12VideoEncodeFrameScheduler currently supports only closed GOP scheduling.");
        }
    }

    void pushFrame(
        std::vector<D3D12VideoEncodeFramePlan>& out,
        uint64_t& encodeIndex,
        uint64_t gopStart,
        uint64_t displayOrder,
        D3D12VideoEncodeFrameType type,
        int64_t list0,
        int64_t list1,
        uint32_t referenceIndexInGop) const {

        D3D12VideoEncodeFramePlan p;
        p.encodeOrderIndex = encodeIndex++;
        p.displayOrderIndex = displayOrder;
        p.frameType = type;
        p.usedAsReference = isReferenceFrame(type);
        p.list0DisplayOrder = list0;
        p.list1DisplayOrder = list1;
        p.gopDisplayIndex = static_cast<uint32_t>(displayOrder - gopStart);
        p.referenceIndexInGop = referenceIndexInGop;
        out.push_back(p);
    }

    void appendGop(
        std::vector<D3D12VideoEncodeFramePlan>& out,
        uint64_t& encodeIndex,
        uint64_t gopStart,
        uint64_t gopEnd) const {

        if (gopStart >= gopEnd) return;

        // First picture of every closed GOP is IDR.
        uint32_t refIndex = 0;
        pushFrame(out, encodeIndex, gopStart, gopStart, D3D12VideoEncodeFrameType::IDR, -1, -1, refIndex);
        uint64_t previousRef = gopStart;

        if (desc_.bFrameCount == 0) {
            for (uint64_t display = gopStart + 1; display < gopEnd; ++display) {
                ++refIndex;
                pushFrame(out, encodeIndex, gopStart, display, D3D12VideoEncodeFrameType::P,
                          static_cast<int64_t>(display - 1), -1, refIndex);
            }
            return;
        }

        while (previousRef + 1 < gopEnd) {
            const uint64_t requestedNextRef = previousRef + static_cast<uint64_t>(desc_.bFrameCount) + 1ull;
            const uint64_t nextRef = std::min<uint64_t>(requestedNextRef, gopEnd - 1);
            ++refIndex;
            pushFrame(out, encodeIndex, gopStart, nextRef, D3D12VideoEncodeFrameType::P,
                      static_cast<int64_t>(previousRef), -1, refIndex);

            for (uint64_t display = previousRef + 1; display < nextRef; ++display) {
                pushFrame(out, encodeIndex, gopStart, display, D3D12VideoEncodeFrameType::B,
                          static_cast<int64_t>(previousRef), static_cast<int64_t>(nextRef), refIndex);
            }

            previousRef = nextRef;
        }
    }

    D3D12VideoEncodeFrameSchedulerDesc desc_ = {};
};

} // namespace D3DVideoEncoderLib
