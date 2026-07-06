#pragma once

#include "backend/d3d12video/ID3D12VideoEncodeBackend.hpp"
#include "util/DebugLog.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderTypes.hpp>

#include <wrl/client.h>

struct ID3D12VideoDevice;
struct ID3D12VideoDevice3;

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

    DebugLog log_;
    D3D12VideoEncoderDesc desc_ = {};
    Microsoft::WRL::ComPtr<ID3D12VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D12VideoDevice3> videoDevice3_;
    D3D12VideoEncodeFormatCapability capability_ = {};
    bool open_ = false;
};

} // namespace D3DVideoEncoderLib
