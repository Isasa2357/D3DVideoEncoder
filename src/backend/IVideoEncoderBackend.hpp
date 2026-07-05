#pragma once

#include "surface/EncodeSurface.hpp"

#include <D3DVideoEncoder/D3D11VideoEncoderDesc.hpp>

namespace D3DVideoEncoderLib {

class IVideoEncoderBackend {
public:
    virtual ~IVideoEncoderBackend() = default;

    virtual void initialize(const D3D11VideoEncoderDesc& desc) = 0;
    virtual VideoPixelFormat requiredInputFormat() const = 0;
    virtual EncodeSurface::Api requiredSurfaceApi() const = 0;

    virtual void encode(
        const EncodeSurface& surface,
        int64_t timestamp100ns,
        int64_t duration100ns) = 0;

    virtual void flush() = 0;
    virtual void close() = 0;
};

} // namespace D3DVideoEncoderLib
