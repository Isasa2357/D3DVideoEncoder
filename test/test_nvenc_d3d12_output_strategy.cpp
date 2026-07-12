#include "backend/nvenc/NvencD3D12OutputStrategy.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace D3DVideoEncoderLib;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    using State = NvencD3D12OutputStrategy::SlotState;
    require(NvencD3D12OutputStrategy::IsValidTransition(State::Free, State::Prepared), "Free -> Prepared must be valid");
    require(NvencD3D12OutputStrategy::IsValidTransition(State::Prepared, State::Submitted), "Prepared -> Submitted must be valid");
    require(NvencD3D12OutputStrategy::IsValidTransition(State::Submitted, State::FenceCompleted), "Submitted -> FenceCompleted must be valid");
    require(NvencD3D12OutputStrategy::IsValidTransition(State::FenceCompleted, State::Locked), "FenceCompleted -> Locked must be valid");
    require(NvencD3D12OutputStrategy::IsValidTransition(State::Locked, State::Free), "Locked -> Free must be valid");
    require(!NvencD3D12OutputStrategy::IsValidTransition(State::Submitted, State::Free), "Submitted slot must not be reused before completion");
    require(!NvencD3D12OutputStrategy::IsValidTransition(State::Locked, State::Locked), "slot must not be locked twice");

    uint64_t fence = 0;
    for (uint64_t expected = 1; expected <= 100; ++expected) {
        fence = NvencD3D12OutputStrategy::NextFenceValue(fence);
        require(fence == expected, "fence values must be strictly monotonic");
    }

    NvencD3D12OutputStrategy strategy(4);
    std::vector<const void*> addresses;
    for (uint32_t i = 0; i < strategy.slotCount(); ++i) {
        require(strategy.slotState(i) == State::Free, "new slot must be Free");
        addresses.push_back(strategy.slotDescriptorAddress(i));
    }
    strategy.release();
    strategy.release();
    for (uint32_t i = 0; i < strategy.slotCount(); ++i) {
        require(strategy.slotDescriptorAddress(i) == addresses[i], "output descriptor pointer must remain stable");
        require(strategy.slotState(i) == State::Free, "idempotent cleanup must leave slots Free");
    }

    std::cout << "NVENC D3D12 output strategy tests passed.\n";
    return 0;
}
