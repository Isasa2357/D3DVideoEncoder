#pragma once

#include "backend/d3d12video/D3D12VideoEncodeBitstreamWriter.hpp"
#include "backend/d3d12video/D3D12VideoEncodeCapabilities.hpp"
#include "backend/d3d12video/ID3D12VideoEncodeBackend.hpp"
#include "util/DebugLog.hpp"

#include <wrl/client.h>

#include <array>
#include <cstdint>

struct ID3D12CommandAllocator;
struct ID3D12CommandQueue;
struct ID3D12Fence;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct ID3D12VideoDevice;
struct ID3D12VideoDevice3;
struct ID3D12VideoEncodeCommandList2;
struct ID3D12VideoEncoder;
struct ID3D12VideoEncoderHeap;

namespace D3DVideoEncoderLib {

class D3D12VideoEncodeBackend final : public ID3D12VideoEncodeBackend {
public:
    explicit D3D12VideoEncodeBackend(DebugLog log = DebugLog());
    ~D3D12VideoEncodeBackend() override;

    void initialize(const D3D12VideoEncoderDesc& desc) override;
    void encode(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns, int64_t duration100ns) override;
    void flush() override;
    void close() override;

private:
    void validateDesc() const;
    void queryVideoDevice();
    void queryEncodeSupport();
    void queryResourceRequirements();
    void createEncoderObjects();
    void createQueuesAndCommands();
    void createBuffers();
    void createReconstructedPictures();
    void createFences();
    void destroyObjects() noexcept;

    void recordEncodeFrame(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, bool restoreInputState);
    void copyOutputsToReadback();
    void waitForCopyQueue();
    void writeResolvedBitstream(int64_t timestamp100ns, int64_t duration100ns);

    void transitionVideo(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    void transitionCopy(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);
    void signalVideoAndWaitOnCopy();

    DebugLog log_;
    D3D12VideoEncoderDesc desc_ = {};
    D3D12VideoEncodeFormatCapability capability_ = {};

    Microsoft::WRL::ComPtr<ID3D12VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D12VideoDevice3> videoDevice3_;
    Microsoft::WRL::ComPtr<ID3D12VideoEncoder> encoder_;
    Microsoft::WRL::ComPtr<ID3D12VideoEncoderHeap> encoderHeap_;

    Microsoft::WRL::ComPtr<ID3D12CommandQueue> videoQueue_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> videoAllocator_;
    Microsoft::WRL::ComPtr<ID3D12VideoEncodeCommandList2> videoCommandList_;

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> copyAllocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> copyCommandList_;

    Microsoft::WRL::ComPtr<ID3D12Fence> videoFence_;
    Microsoft::WRL::ComPtr<ID3D12Fence> copyFence_;
    uint64_t videoFenceValue_ = 0;
    uint64_t copyFenceValue_ = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> bitstreamBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> bitstreamReadback_;
    Microsoft::WRL::ComPtr<ID3D12Resource> encoderMetadataBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> resolvedMetadataBuffer_;
    Microsoft::WRL::ComPtr<ID3D12Resource> resolvedMetadataReadback_;
    std::array<Microsoft::WRL::ComPtr<ID3D12Resource>, 2> reconstructedPictures_;

    D3D12_RESOURCE_STATES bitstreamState_ = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES encoderMetadataState_ = D3D12_RESOURCE_STATE_COMMON;
    D3D12_RESOURCE_STATES resolvedMetadataState_ = D3D12_RESOURCE_STATE_COMMON;
    std::array<D3D12_RESOURCE_STATES, 2> reconstructedStates_ = {
        D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_STATE_COMMON
    };

    uint64_t bitstreamBufferSize_ = 0;
    uint64_t encoderMetadataBufferSize_ = 0;
    uint64_t resolvedMetadataBufferSize_ = 0;
    uint32_t bitstreamAlignment_ = 1;
    uint32_t metadataAlignment_ = 1;

    uint64_t frameIndex_ = 0;
    uint32_t currentReconIndex_ = 0;
    uint32_t previousReconIndex_ = 0;
    bool hasReferenceFrame_ = false;

    D3D12VideoEncodeBitstreamWriter writer_;
    bool open_ = false;
};

} // namespace D3DVideoEncoderLib
