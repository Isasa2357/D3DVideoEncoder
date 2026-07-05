#pragma once

#include "backend/IVideoEncoderBackend.hpp"
#include "backend/nvenc/NvencCommon.hpp"
#include "util/DebugLog.hpp"

namespace D3DVideoEncoderLib {

class NvencD3D11EncoderBackend final : public IVideoEncoderBackend {
public:
    explicit NvencD3D11EncoderBackend(DebugLog log = DebugLog());
    ~NvencD3D11EncoderBackend() override;

    void initialize(const D3D11VideoEncoderDesc& desc) override;

    VideoPixelFormat requiredInputFormat() const override { return desc_.internalFormat; }
    EncodeSurface::Api requiredSurfaceApi() const override { return EncodeSurface::Api::D3D11; }

    void encode(const EncodeSurface& surface, int64_t timestamp100ns, int64_t duration100ns) override;
    void flush() override;
    void close() override;

private:
    DebugLog log_;
    D3D11VideoEncoderDesc desc_ = {};
    NvencEncoderSession session_;
    bool open_ = false;
};

} // namespace D3DVideoEncoderLib
