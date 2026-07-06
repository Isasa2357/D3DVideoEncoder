#pragma once

#include "backend/d3d12video/D3D12VideoEncodeBitstreamWriter.hpp"
#include "backend/d3d12video/D3D12VideoEncodeCapabilities.hpp"
#include "backend/d3d12video/ID3D12VideoEncodeBackend.hpp"
#include "util/DebugLog.hpp"

#include <D3D12Helper/D3D12Core/D3D12CommandContext.hpp>
#include <D3D12Helper/D3D12Framework/D3D12DescriptorAllocator.hpp>
#include <D3D12Helper/D3D12Framework/D3D12Resource.hpp>
#include <D3D12Helper/D3D12Processing/D3D12Processing.hpp>

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
    void validateDesc();
    void queryVideoDevice();
    void queryEncodeSupport();
    void queryResourceRequirements();
    void initializeProcessingIfNeeded();
    void createEncoderObjects();
    void createQueuesAndCommands();
    void createBuffers();
    void createReconstructedPictures();
    void createFences();
    void destroyObjects() noexcept;

    bool inputAlreadyMatchesInternalFormat() const noexcept;
    uint32_t sourceWidth() const noexcept;
    uint32_t sourceHeight() const noexcept;
    D3D12CoreLib::Processing::ProcessingRect resolvedSourceRect() const;
    bool needsResizeOrCrop() const;
    bool inputIsRgbaLike() const noexcept;
    D3D12CoreLib::Processing::ProcessingFilter processingFilter() const noexcept;
    void validateInputResource(ID3D12Resource* resource) const;
    ID3D12Resource* convertToInternalFormat(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState);

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

    bool useProcessing_ = false;
    D3D12CoreLib::D3D12DescriptorAllocator cbvSrvUavAllocator_;
    D3D12CoreLib::D3D12DescriptorAllocator samplerAllocator_;
    D3D12CoreLib::Processing::D3D12ProcessingContext processingContext_;
    D3D12CoreLib::Processing::D3D12FormatConverter formatConverter_;
    D3D12CoreLib::Processing::D3D12Resizer resizer_;
    D3D12CoreLib::Processing::D3D12FusedProcessor fusedProcessor_;
    D3D12CoreLib::D3D12CommandContext processingCommandContext_;
    D3D12CoreLib::D3D12Resource resizedTexture_;
    D3D12_RESOURCE_STATES resizedTextureState_ = D3D12_RESOURCE_STATE_COMMON;
    D3D12CoreLib::D3D12Resource convertedTexture_;
    D3D12_RESOURCE_STATES convertedTextureState_ = D3D12_RESOURCE_STATE_COMMON;
    UINT64 processingFenceValue_ = 0;
    bool open_ = false;
};

} // namespace D3DVideoEncoderLib
