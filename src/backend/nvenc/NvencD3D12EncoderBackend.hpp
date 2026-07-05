#pragma once

#include "backend/nvenc/INvencD3D12EncoderBackend.hpp"
#include "backend/nvenc/NvencCommon.hpp"
#include "util/DebugLog.hpp"

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
    void validateResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState) const;

    DebugLog log_;
    D3D12VideoEncoderDesc desc_ = {};
    NvencEncoderSession session_;
    bool open_ = false;
};

} // namespace D3DVideoEncoderLib
