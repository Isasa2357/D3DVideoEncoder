#pragma once

#include <D3DVideoEncoder/D3D12VideoEncoderDesc.hpp>

#include <d3d12.h>
#include <cstdint>

namespace D3DVideoEncoderLib {

class ID3D12VideoEncodeBackend {
public:
    virtual ~ID3D12VideoEncodeBackend() = default;
    virtual void initialize(const D3D12VideoEncoderDesc& desc) = 0;
    virtual void encode(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns, int64_t duration100ns) = 0;
    virtual void flush() = 0;
    virtual void close() = 0;
};

} // namespace D3DVideoEncoderLib
