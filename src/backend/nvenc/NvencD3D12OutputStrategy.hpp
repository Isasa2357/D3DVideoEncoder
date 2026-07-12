#pragma once

#include "backend/mux/NvencOutputMuxer.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderTypes.hpp>

#include <Windows.h>
#include <d3d12.h>
#include <nvEncodeAPI.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace D3DVideoEncoderLib {

class NvencD3D12OutputStrategy {
public:
    enum class SlotState {
        Free,
        Prepared,
        Submitted,
        FenceCompleted,
        Locked
    };

    struct PreparedFrame {
        uint32_t slotIndex = 0;
        NV_ENC_INPUT_RESOURCE_D3D12* input = nullptr;
        NV_ENC_OUTPUT_RESOURCE_D3D12* output = nullptr;
    };

    explicit NvencD3D12OutputStrategy(uint32_t slotCount);
    ~NvencD3D12OutputStrategy();

    NvencD3D12OutputStrategy(const NvencD3D12OutputStrategy&) = delete;
    NvencD3D12OutputStrategy& operator=(const NvencD3D12OutputStrategy&) = delete;

    void initialize(
        ID3D12Device* device,
        void* encoder,
        NV_ENCODE_API_FUNCTION_LIST* functions,
        uint32_t width,
        uint32_t height,
        VideoPixelFormat inputFormat,
        bool traceEnabled);

    PreparedFrame prepare(
        NV_ENC_REGISTERED_PTR registeredInput,
        uint64_t frameIndex,
        int64_t timestamp100ns,
        int64_t duration100ns);
    NV_ENC_FENCE_POINT_D3D12 beginInputRegistration();
    void waitForInputRegistration(uint64_t fenceValue);
    void commit(const PreparedFrame& frame);
    void rollback(const PreparedFrame& frame) noexcept;
    void drainNext(NvencOutputMuxer& muxer);
    void drainAll(NvencOutputMuxer& muxer);
    void release() noexcept;

    uint64_t submittedCount() const noexcept { return submittedCount_; }
    uint64_t receivedCount() const noexcept { return receivedCount_; }
    uint64_t outstandingCount() const noexcept { return submittedCount_ - receivedCount_; }
    uint32_t slotCount() const noexcept { return static_cast<uint32_t>(slots_.size()); }
    SlotState slotState(uint32_t index) const;
    uint64_t slotFenceValue(uint32_t index) const;
    const void* slotDescriptorAddress(uint32_t index) const;

    static bool IsValidTransition(SlotState before, SlotState after) noexcept;
    static uint64_t NextFenceValue(uint64_t current);

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12Resource> outputResource;
        NV_ENC_REGISTERED_PTR registeredOutput = nullptr;
        NV_ENC_INPUT_PTR mappedInput = nullptr;
        NV_ENC_OUTPUT_PTR mappedOutput = nullptr;
        NV_ENC_INPUT_RESOURCE_D3D12 input = {};
        NV_ENC_OUTPUT_RESOURCE_D3D12 output = {};
        SlotState state = SlotState::Free;
        uint64_t fenceValue = 0;
        uint64_t frameIndex = 0;
        int64_t timestamp100ns = 0;
        int64_t duration100ns = 0;
        bool inputMapped = false;
        bool outputMapped = false;
        bool outputRegistered = false;
        bool locked = false;
    };

    void transition(Slot& slot, SlotState after);
    void waitForFence(Slot& slot);
    void unmapSlot(Slot& slot) noexcept;
    void traceSlot(const char* operation, uint32_t index, const Slot& slot) const;
    static uint64_t outputBufferSize(uint32_t width, uint32_t height, VideoPixelFormat inputFormat);

    std::vector<Slot> slots_;
    Microsoft::WRL::ComPtr<ID3D12Fence> outputFence_;
    Microsoft::WRL::ComPtr<ID3D12Fence> inputFence_;
    HANDLE waitEvent_ = nullptr;
    ID3D12Device* device_ = nullptr;
    void* encoder_ = nullptr;
    NV_ENCODE_API_FUNCTION_LIST* functions_ = nullptr;
    bool traceEnabled_ = false;
    uint64_t nextFenceValue_ = 0;
    uint64_t inputFenceValue_ = 0;
    uint64_t submittedCount_ = 0;
    uint64_t receivedCount_ = 0;
};

} // namespace D3DVideoEncoderLib
