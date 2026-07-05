#pragma once

#include "backend/IVideoEncoderBackend.hpp"
#include "backend/MediaFoundationRuntime.hpp"
#include "util/DebugLog.hpp"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

namespace D3DVideoEncoderLib {

class MediaFoundationEncoderBackend final : public IVideoEncoderBackend {
public:
    explicit MediaFoundationEncoderBackend(DebugLog log = DebugLog());
    ~MediaFoundationEncoderBackend() override;

    void initialize(const D3D11VideoEncoderDesc& desc) override;

    VideoPixelFormat requiredInputFormat() const override { return desc_.internalFormat; }
    EncodeSurface::Api requiredSurfaceApi() const override { return EncodeSurface::Api::D3D11; }

    void encode(
        const EncodeSurface& surface,
        int64_t timestamp100ns,
        int64_t duration100ns) override;

    void flush() override;
    void close() override;

private:
    GUID codecSubtype() const;
    GUID inputSubtype() const;
    std::string describeConfiguration() const;
    void verifyCapabilities() const;
    void configureOutputType(IMFMediaType* mediaType) const;
    void configureInputType(IMFMediaType* mediaType) const;

    DebugLog log_;
    D3D11VideoEncoderDesc desc_ = {};
    MediaFoundationRuntime runtime_;

    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgiDeviceManager_;
    Microsoft::WRL::ComPtr<IMFSinkWriter> sinkWriter_;

    UINT resetToken_ = 0;
    DWORD streamIndex_ = 0;
    bool open_ = false;
};

} // namespace D3DVideoEncoderLib
