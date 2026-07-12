#include "backend/nvenc/NvencD3D12OutputStrategy.hpp"

#include "backend/nvenc/NvencCommon.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

#include <algorithm>
#include <iostream>
#include <limits>
#include <sstream>

namespace D3DVideoEncoderLib {

namespace {

const char* StateName(NvencD3D12OutputStrategy::SlotState state) noexcept {
    switch (state) {
    case NvencD3D12OutputStrategy::SlotState::Free: return "Free";
    case NvencD3D12OutputStrategy::SlotState::Prepared: return "Prepared";
    case NvencD3D12OutputStrategy::SlotState::Submitted: return "Submitted";
    case NvencD3D12OutputStrategy::SlotState::FenceCompleted: return "FenceCompleted";
    case NvencD3D12OutputStrategy::SlotState::Locked: return "Locked";
    default: return "Unknown";
    }
}

} // namespace

NvencD3D12OutputStrategy::NvencD3D12OutputStrategy(uint32_t slotCount) {
    if (slotCount == 0) {
        throw D3DVideoEncoderError("NVENC D3D12 output strategy requires at least one slot.");
    }
    slots_.resize(slotCount);
}

NvencD3D12OutputStrategy::~NvencD3D12OutputStrategy() {
    release();
}

bool NvencD3D12OutputStrategy::IsValidTransition(SlotState before, SlotState after) noexcept {
    switch (before) {
    case SlotState::Free: return after == SlotState::Prepared;
    case SlotState::Prepared: return after == SlotState::Submitted || after == SlotState::Free;
    case SlotState::Submitted: return after == SlotState::FenceCompleted;
    case SlotState::FenceCompleted: return after == SlotState::Locked;
    case SlotState::Locked: return after == SlotState::Free;
    default: return false;
    }
}

uint64_t NvencD3D12OutputStrategy::NextFenceValue(uint64_t current) {
    if (current == std::numeric_limits<uint64_t>::max()) {
        throw D3DVideoEncoderError("NVENC D3D12 output fence value overflow.");
    }
    return current + 1;
}

uint64_t NvencD3D12OutputStrategy::outputBufferSize(uint32_t width, uint32_t height, VideoPixelFormat inputFormat) {
    const uint64_t bytesPerLumaSample = inputFormat == VideoPixelFormat::P010 ? 2u : 1u;
    const uint64_t yuvBytes = static_cast<uint64_t>(width) * height * 3u * bytesPerLumaSample / 2u;
    const uint64_t doubled = yuvBytes * 2u;
    return (doubled + 3u) & ~uint64_t{3u};
}

void NvencD3D12OutputStrategy::initialize(
    ID3D12Device* device,
    void* encoder,
    NV_ENCODE_API_FUNCTION_LIST* functions,
    uint32_t width,
    uint32_t height,
    VideoPixelFormat inputFormat,
    bool traceEnabled) {
    if (!device || !encoder || !functions) {
        throw D3DVideoEncoderError("NVENC D3D12 output strategy received an invalid initialization argument.");
    }

    device_ = device;
    encoder_ = encoder;
    functions_ = functions;
    traceEnabled_ = traceEnabled;

    HRESULT hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&outputFence_));
    if (FAILED(hr)) {
        throw D3DVideoEncoderError("NVENC D3D12 failed to create the persistent output fence.");
    }
    hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&inputFence_));
    if (FAILED(hr)) {
        release();
        throw D3DVideoEncoderError("NVENC D3D12 failed to create the persistent input fence.");
    }
    waitEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!waitEvent_) {
        release();
        throw D3DVideoEncoderError("NVENC D3D12 failed to create the persistent output wait event.");
    }

    const uint64_t bufferSize = outputBufferSize(width, height, inputFormat);
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;

    D3D12_RESOURCE_DESC resource = {};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource.Width = bufferSize;
    resource.Height = 1;
    resource.DepthOrArraySize = 1;
    resource.MipLevels = 1;
    resource.Format = DXGI_FORMAT_UNKNOWN;
    resource.SampleDesc.Count = 1;
    resource.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource.Flags = D3D12_RESOURCE_FLAG_NONE;

    try {
        for (uint32_t i = 0; i < slots_.size(); ++i) {
            Slot& slot = slots_[i];
            hr = device_->CreateCommittedResource(
                &heap,
                D3D12_HEAP_FLAG_NONE,
                &resource,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                IID_PPV_ARGS(&slot.outputResource));
            if (FAILED(hr)) {
                throw D3DVideoEncoderError("NVENC D3D12 failed to create a READBACK output resource.");
            }

            NV_ENC_REGISTER_RESOURCE registration = {};
            registration.version = NV_ENC_REGISTER_RESOURCE_VER;
            registration.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
            registration.resourceToRegister = slot.outputResource.Get();
            registration.width = static_cast<uint32_t>(bufferSize);
            registration.height = 1;
            registration.pitch = 0;
            registration.bufferFormat = NV_ENC_BUFFER_FORMAT_U8;
            registration.bufferUsage = NV_ENC_OUTPUT_BITSTREAM;
            const NVENCSTATUS status = functions_->nvEncRegisterResource(encoder_, &registration);
            D3DVE_THROW_IF_NVENC_FAILED_MSG(status, "NVENC D3D12 output nvEncRegisterResource");
            slot.registeredOutput = registration.registeredResource;
            slot.outputRegistered = true;

            slot.input = {};
            slot.input.version = NV_ENC_INPUT_RESOURCE_D3D12_VER;
            slot.input.inputFencePoint.version = NV_ENC_FENCE_POINT_D3D12_VER;

            slot.output = {};
            slot.output.version = NV_ENC_OUTPUT_RESOURCE_D3D12_VER;
            slot.output.outputFencePoint.version = NV_ENC_FENCE_POINT_D3D12_VER;
            slot.output.outputFencePoint.pFence = outputFence_.Get();
            traceSlot("initialized", i, slot);
        }
    } catch (...) {
        release();
        throw;
    }
}

NvencD3D12OutputStrategy::PreparedFrame NvencD3D12OutputStrategy::prepare(
    NV_ENC_REGISTERED_PTR registeredInput,
    uint64_t frameIndex,
    int64_t timestamp100ns,
    int64_t duration100ns) {
    if (!encoder_ || !functions_ || !registeredInput) {
        throw D3DVideoEncoderError("NVENC D3D12 output prepare received an invalid resource.");
    }
    const uint32_t index = static_cast<uint32_t>(submittedCount_ % slots_.size());
    Slot& slot = slots_[index];
    if (slot.state != SlotState::Free || slot.inputMapped || slot.outputMapped || slot.locked) {
        throw D3DVideoEncoderError("NVENC D3D12 output slot was reused before completion.");
    }

    transition(slot, SlotState::Prepared);
    slot.frameIndex = frameIndex;
    slot.timestamp100ns = timestamp100ns;
    slot.duration100ns = duration100ns;

    try {
        NV_ENC_MAP_INPUT_RESOURCE mapInput = {};
        mapInput.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mapInput.registeredResource = registeredInput;
        D3DVE_THROW_IF_NVENC_FAILED_MSG(
            functions_->nvEncMapInputResource(encoder_, &mapInput),
            "NVENC D3D12 input nvEncMapInputResource");
        slot.mappedInput = mapInput.mappedResource;
        slot.inputMapped = true;

        NV_ENC_MAP_INPUT_RESOURCE mapOutput = {};
        mapOutput.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mapOutput.registeredResource = slot.registeredOutput;
        D3DVE_THROW_IF_NVENC_FAILED_MSG(
            functions_->nvEncMapInputResource(encoder_, &mapOutput),
            "NVENC D3D12 output nvEncMapInputResource");
        slot.mappedOutput = reinterpret_cast<NV_ENC_OUTPUT_PTR>(mapOutput.mappedResource);
        slot.outputMapped = true;

        slot.fenceValue = NextFenceValue(nextFenceValue_);
        nextFenceValue_ = slot.fenceValue;

        slot.input.pInputBuffer = slot.mappedInput;
        slot.input.inputFencePoint.pFence = inputFence_.Get();
        slot.input.inputFencePoint.waitValue = inputFenceValue_;
        slot.input.inputFencePoint.signalValue = 0;
        slot.input.inputFencePoint.bWait = 1;
        slot.input.inputFencePoint.bSignal = 0;

        slot.output.pOutputBuffer = slot.mappedOutput;
        slot.output.outputFencePoint.pFence = outputFence_.Get();
        slot.output.outputFencePoint.waitValue = 0;
        slot.output.outputFencePoint.signalValue = slot.fenceValue;
        slot.output.outputFencePoint.bWait = 0;
        slot.output.outputFencePoint.bSignal = 1;
        traceSlot("prepared", index, slot);
    } catch (...) {
        unmapSlot(slot);
        if (slot.state == SlotState::Prepared) {
            transition(slot, SlotState::Free);
        }
        throw;
    }

    return PreparedFrame{index, &slot.input, &slot.output};
}

NV_ENC_FENCE_POINT_D3D12 NvencD3D12OutputStrategy::beginInputRegistration() {
    if (!inputFence_) {
        throw D3DVideoEncoderError("NVENC D3D12 input registration fence is not initialized.");
    }
    NV_ENC_FENCE_POINT_D3D12 point = {};
    point.version = NV_ENC_FENCE_POINT_D3D12_VER;
    point.pFence = inputFence_.Get();
    point.waitValue = inputFenceValue_;
    point.bWait = inputFenceValue_ > 0 ? 1u : 0u;
    inputFenceValue_ = NextFenceValue(inputFenceValue_);
    point.signalValue = inputFenceValue_;
    point.bSignal = 1;
    return point;
}

void NvencD3D12OutputStrategy::waitForInputRegistration(uint64_t fenceValue) {
    if (inputFence_->GetCompletedValue() >= fenceValue) return;
    const HRESULT hr = inputFence_->SetEventOnCompletion(fenceValue, waitEvent_);
    if (FAILED(hr)) {
        throw D3DVideoEncoderError("NVENC D3D12 input registration SetEventOnCompletion failed.");
    }
    const DWORD wait = WaitForSingleObject(waitEvent_, 30000);
    if (wait == WAIT_TIMEOUT) {
        throw D3DVideoEncoderError("NVENC D3D12 input registration fence wait timed out.");
    }
    if (wait != WAIT_OBJECT_0) {
        throw D3DVideoEncoderError("NVENC D3D12 input registration fence wait failed.");
    }
}

void NvencD3D12OutputStrategy::commit(const PreparedFrame& frame) {
    if (frame.slotIndex >= slots_.size()) {
        throw D3DVideoEncoderError("NVENC D3D12 output commit received an invalid slot.");
    }
    Slot& slot = slots_[frame.slotIndex];
    if (frame.input != &slot.input || frame.output != &slot.output) {
        throw D3DVideoEncoderError("NVENC D3D12 output descriptor pointer changed before submission.");
    }
    transition(slot, SlotState::Submitted);
    ++submittedCount_;
    traceSlot("submitted", frame.slotIndex, slot);
}

void NvencD3D12OutputStrategy::rollback(const PreparedFrame& frame) noexcept {
    if (frame.slotIndex >= slots_.size()) return;
    Slot& slot = slots_[frame.slotIndex];
    unmapSlot(slot);
    if (slot.state == SlotState::Prepared) {
        slot.state = SlotState::Free;
    }
    traceSlot("rolled back", frame.slotIndex, slot);
}

void NvencD3D12OutputStrategy::waitForFence(Slot& slot) {
    if (outputFence_->GetCompletedValue() < slot.fenceValue) {
        const HRESULT hr = outputFence_->SetEventOnCompletion(slot.fenceValue, waitEvent_);
        if (FAILED(hr)) {
            throw D3DVideoEncoderError("NVENC D3D12 output SetEventOnCompletion failed.");
        }
        const DWORD wait = WaitForSingleObject(waitEvent_, 30000);
        if (wait == WAIT_TIMEOUT) {
            throw D3DVideoEncoderError("NVENC D3D12 output fence wait timed out.");
        }
        if (wait != WAIT_OBJECT_0) {
            throw D3DVideoEncoderError("NVENC D3D12 output fence wait failed.");
        }
    }
    transition(slot, SlotState::FenceCompleted);
}

void NvencD3D12OutputStrategy::drainNext(NvencOutputMuxer& muxer) {
    if (receivedCount_ >= submittedCount_) {
        throw D3DVideoEncoderError("NVENC D3D12 drain requested with no outstanding output slot.");
    }
    const uint32_t index = static_cast<uint32_t>(receivedCount_ % slots_.size());
    Slot& slot = slots_[index];
    if (slot.state != SlotState::Submitted) {
        throw D3DVideoEncoderError("NVENC D3D12 output drain encountered an invalid slot state.");
    }

    waitForFence(slot);
    traceSlot("fence completed", index, slot);

    NV_ENC_LOCK_BITSTREAM lock = {};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = &slot.output;
    lock.doNotWait = 0;
    D3DVE_THROW_IF_NVENC_FAILED_MSG(
        functions_->nvEncLockBitstream(encoder_, &lock),
        "NVENC D3D12 nvEncLockBitstream");
    slot.locked = true;
    transition(slot, SlotState::Locked);
    traceSlot("locked", index, slot);

    try {
        if (lock.bitstreamBufferPtr && lock.bitstreamSizeInBytes > 0) {
            muxer.writeAccessUnit(
                static_cast<const uint8_t*>(lock.bitstreamBufferPtr),
                static_cast<size_t>(lock.bitstreamSizeInBytes),
                slot.timestamp100ns,
                slot.duration100ns);
        }
        D3DVE_THROW_IF_NVENC_FAILED_MSG(
            functions_->nvEncUnlockBitstream(encoder_, &slot.output),
            "NVENC D3D12 nvEncUnlockBitstream");
        slot.locked = false;
    } catch (...) {
        if (slot.locked) {
            functions_->nvEncUnlockBitstream(encoder_, &slot.output);
            slot.locked = false;
        }
        unmapSlot(slot);
        slot.state = SlotState::Free;
        throw;
    }

    unmapSlot(slot);
    transition(slot, SlotState::Free);
    ++receivedCount_;
    traceSlot("released", index, slot);
}

void NvencD3D12OutputStrategy::drainAll(NvencOutputMuxer& muxer) {
    while (receivedCount_ < submittedCount_) {
        drainNext(muxer);
    }
}

void NvencD3D12OutputStrategy::unmapSlot(Slot& slot) noexcept {
    if (encoder_ && functions_) {
        if (slot.inputMapped && slot.mappedInput) {
            functions_->nvEncUnmapInputResource(encoder_, slot.mappedInput);
        }
        if (slot.outputMapped && slot.mappedOutput) {
            functions_->nvEncUnmapInputResource(encoder_, slot.mappedOutput);
        }
    }
    slot.mappedInput = nullptr;
    slot.mappedOutput = nullptr;
    slot.inputMapped = false;
    slot.outputMapped = false;
    slot.input.pInputBuffer = nullptr;
    slot.output.pOutputBuffer = nullptr;
}

void NvencD3D12OutputStrategy::release() noexcept {
    for (Slot& slot : slots_) {
        if (encoder_ && functions_ && slot.locked) {
            functions_->nvEncUnlockBitstream(encoder_, &slot.output);
            slot.locked = false;
        }
        unmapSlot(slot);
        if (encoder_ && functions_ && slot.outputRegistered && slot.registeredOutput) {
            functions_->nvEncUnregisterResource(encoder_, slot.registeredOutput);
        }
        slot.registeredOutput = nullptr;
        slot.outputRegistered = false;
        slot.outputResource.Reset();
        slot.state = SlotState::Free;
    }
    if (waitEvent_) {
        CloseHandle(waitEvent_);
        waitEvent_ = nullptr;
    }
    outputFence_.Reset();
    inputFence_.Reset();
    device_ = nullptr;
    encoder_ = nullptr;
    functions_ = nullptr;
}

void NvencD3D12OutputStrategy::transition(Slot& slot, SlotState after) {
    if (!IsValidTransition(slot.state, after)) {
        std::ostringstream message;
        message << "NVENC D3D12 invalid output slot transition "
                << StateName(slot.state) << " -> " << StateName(after) << ".";
        throw D3DVideoEncoderError(message.str());
    }
    slot.state = after;
}

void NvencD3D12OutputStrategy::traceSlot(const char* operation, uint32_t index, const Slot& slot) const {
    if (!traceEnabled_) return;
    std::cerr << "[NVENC D3D12 OUTPUT] " << operation
              << " slot=" << index
              << " state=" << StateName(slot.state)
              << " frame=" << slot.frameIndex
              << " fence=" << slot.fenceValue
              << " submitted=" << submittedCount_
              << " received=" << receivedCount_
              << std::endl;
}

NvencD3D12OutputStrategy::SlotState NvencD3D12OutputStrategy::slotState(uint32_t index) const {
    return slots_.at(index).state;
}

uint64_t NvencD3D12OutputStrategy::slotFenceValue(uint32_t index) const {
    return slots_.at(index).fenceValue;
}

const void* NvencD3D12OutputStrategy::slotDescriptorAddress(uint32_t index) const {
    return &slots_.at(index).output;
}

} // namespace D3DVideoEncoderLib
