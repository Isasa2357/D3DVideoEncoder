#pragma once

#include "backend/IVideoEncoderBackend.hpp"
#include "util/DebugLog.hpp"

namespace D3DVideoEncoderLib {

class UnsupportedEncoderBackend final : public IVideoEncoderBackend {
public:
    UnsupportedEncoderBackend(D3DVideoEncoderBackendType backendType, DebugLog log);

    void initialize(const D3D11VideoEncoderDesc& desc) override;
    VideoPixelFormat requiredInputFormat() const override { return desc_.internalFormat; }
    EncodeSurface::Api requiredSurfaceApi() const override;

    void encode(const EncodeSurface& surface, int64_t timestamp100ns, int64_t duration100ns) override;
    void flush() override;
    void close() override;

private:
    [[noreturn]] void throwUnsupported() const;

    D3DVideoEncoderBackendType backendType_ = D3DVideoEncoderBackendType::MediaFoundation;
    D3D11VideoEncoderDesc desc_ = {};
    DebugLog log_;
};

} // namespace D3DVideoEncoderLib
