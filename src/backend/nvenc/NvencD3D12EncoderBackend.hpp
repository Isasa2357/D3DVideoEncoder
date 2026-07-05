#pragma once

#include "backend/nvenc/INvencD3D12EncoderBackend.hpp"
#include "backend/nvenc/NvencCommon.hpp"
#include "util/DebugLog.hpp"

#include <D3D12Helper/D3D12Core/D3D12CommandContext.hpp>
#include <D3D12Helper/D3D12Framework/D3D12DescriptorAllocator.hpp>
#include <D3D12Helper/D3D12Framework/D3D12Resource.hpp>
#include <D3D12Helper/D3D12Processing/D3D12Processing.hpp>

namespace D3DVideoEncoderLib {

class NvencD3D12EncoderBackend final : public INvencD3D12EncoderBackend {
public:
    explicit NvencD3D12EncoderBackend(DebugLog log = DebugLog());
    ~NvencD3D12EncoderBackend() override;

    void initialize(const D3D12VideoEncoderDesc& desc) override;
    void encode(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns, int64_t duration100ns) override;
    void flush() override;
    void close() override;

private:
    bool inputAlreadyMatchesInternalFormat() const noexcept;
    void validateInputResource(ID3D12Resource* resource) const;
    void validateDirectNvencResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState) const;
    void initializeProcessingIfNeeded();
    ID3D12Resource* convertToInternalFormat(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState);

    DebugLog log_;
    D3D12VideoEncoderDesc desc_ = {};
    NvencEncoderSession session_;

    bool open_ = false;
    bool useProcessing_ = false;

    D3D12CoreLib::D3D12DescriptorAllocator cbvSrvUavAllocator_;
    D3D12CoreLib::D3D12DescriptorAllocator samplerAllocator_;
    D3D12CoreLib::Processing::D3D12ProcessingContext processingContext_;
    D3D12CoreLib::Processing::D3D12FormatConverter formatConverter_;
    D3D12CoreLib::D3D12CommandContext commandContext_;
    D3D12CoreLib::D3D12Resource convertedTexture_;
    D3D12_RESOURCE_STATES convertedTextureState_ = D3D12_RESOURCE_STATE_COMMON;
    UINT64 processingFenceValue_ = 0;
};

} // namespace D3DVideoEncoderLib
