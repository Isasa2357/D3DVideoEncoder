#include <D3DVideoEncoder/D3D11VideoEncoder.hpp>

#include "D3D11VideoEncoderImpl.hpp"

#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <objbase.h>
#include <wrl/client.h>

#include <utility>

namespace D3DVideoEncoderLib {

namespace {

class ComApartmentForQuery {
public:
    ComApartmentForQuery() {
        hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr_) && hr_ != RPC_E_CHANGED_MODE) {
            startupSucceeded_ = false;
        }
    }

    ~ComApartmentForQuery() {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }

    bool startupSucceeded() const noexcept { return startupSucceeded_; }
    HRESULT hr() const noexcept { return hr_; }

private:
    HRESULT hr_ = S_OK;
    bool startupSucceeded_ = true;
};

GUID MediaFoundationCodecSubtype(VideoCodec codec) noexcept {
    switch (codec) {
    case VideoCodec::H264:
        return MFVideoFormat_H264;
    case VideoCodec::HEVC:
        return MFVideoFormat_HEVC;
    case VideoCodec::AV1:
#ifdef MFVideoFormat_AV1
        return MFVideoFormat_AV1;
#else
        return GUID_NULL;
#endif
    default:
        return GUID_NULL;
    }
}

GUID MediaFoundationInputSubtype(VideoPixelFormat format) noexcept {
    switch (format) {
    case VideoPixelFormat::NV12:
        return MFVideoFormat_NV12;
    case VideoPixelFormat::P010:
        return MFVideoFormat_P010;
    default:
        return GUID_NULL;
    }
}

void ReleaseActivateArray(IMFActivate** activates, UINT32 count) noexcept {
    if (!activates) return;
    for (UINT32 i = 0; i < count; ++i) {
        if (activates[i]) {
            activates[i]->Release();
        }
    }
    CoTaskMemFree(activates);
}

std::wstring ReadFriendlyName(IMFActivate* activate) {
    if (!activate) return {};

    wchar_t* name = nullptr;
    UINT32 nameLength = 0;
    if (FAILED(activate->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &nameLength)) || !name) {
        return {};
    }

    std::wstring result(name, nameLength);
    CoTaskMemFree(name);
    return result;
}

void EnumerateMediaFoundationEncoders(
    D3D11MediaFoundationFormatCapability& out,
    UINT32 flags,
    bool hardwareOnly) {

    const GUID inputSubtype = MediaFoundationInputSubtype(out.inputFormat);
    const GUID outputSubtype = MediaFoundationCodecSubtype(out.codec);
    if (IsEqualGUID(inputSubtype, GUID_NULL) || IsEqualGUID(outputSubtype, GUID_NULL)) {
        if (hardwareOnly) {
            out.hardwareQueryHr = E_INVALIDARG;
        } else {
            out.queryHr = E_INVALIDARG;
        }
        return;
    }

    MFT_REGISTER_TYPE_INFO inputInfo = {};
    inputInfo.guidMajorType = MFMediaType_Video;
    inputInfo.guidSubtype = inputSubtype;

    MFT_REGISTER_TYPE_INFO outputInfo = {};
    outputInfo.guidMajorType = MFMediaType_Video;
    outputInfo.guidSubtype = outputSubtype;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    const HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_ENCODER,
        flags,
        &inputInfo,
        &outputInfo,
        &activates,
        &count);

    if (hardwareOnly) {
        out.hardwareQueryHr = hr;
        if (SUCCEEDED(hr)) {
            out.hardwareEncoderCount = count;
            out.hardwareSupported = count > 0;
            if (count > 0) {
                out.firstHardwareEncoderName = ReadFriendlyName(activates[0]);
            }
        }
    } else {
        out.queryHr = hr;
        if (SUCCEEDED(hr)) {
            out.encoderCount = count;
            out.supported = count > 0;
            if (count > 0) {
                out.firstEncoderName = ReadFriendlyName(activates[0]);
            }
        }
    }

    ReleaseActivateArray(activates, count);
}

} // namespace

D3D11VideoEncoder::D3D11VideoEncoder(const D3D11VideoEncoderDesc& desc)
    : impl_(std::make_unique<Impl>(desc)) {}

D3D11VideoEncoder::~D3D11VideoEncoder() = default;
D3D11VideoEncoder::D3D11VideoEncoder(D3D11VideoEncoder&&) noexcept = default;
D3D11VideoEncoder& D3D11VideoEncoder::operator=(D3D11VideoEncoder&&) noexcept = default;

void D3D11VideoEncoder::write(ID3D11Texture2D* texture) {
    impl_->write(texture);
}

void D3D11VideoEncoder::write(ID3D11Texture2D* texture, int64_t timestamp100ns) {
    impl_->write(texture, timestamp100ns);
}

void D3D11VideoEncoder::flush() {
    impl_->flush();
}

void D3D11VideoEncoder::close() {
    impl_->close();
}

bool D3D11VideoEncoder::isOpen() const noexcept {
    return impl_ && impl_->isOpen();
}

uint64_t D3D11VideoEncoder::writtenFrameCount() const noexcept {
    return impl_ ? impl_->writtenFrameCount() : 0;
}

D3D11MediaFoundationFormatCapability D3D11VideoEncoder::QueryMediaFoundationSupport(
    VideoCodec codec,
    VideoPixelFormat inputFormat) {

    D3D11MediaFoundationFormatCapability result;
    result.codec = codec;
    result.inputFormat = inputFormat;

    ComApartmentForQuery com;
    if (!com.startupSucceeded()) {
        result.queryHr = com.hr();
        result.hardwareQueryHr = com.hr();
        return result;
    }

    const HRESULT mfHr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(mfHr)) {
        result.queryHr = mfHr;
        result.hardwareQueryHr = mfHr;
        return result;
    }

    constexpr UINT32 kAllEncoderFlags =
        MFT_ENUM_FLAG_SYNCMFT |
        MFT_ENUM_FLAG_ASYNCMFT |
        MFT_ENUM_FLAG_LOCALMFT |
        MFT_ENUM_FLAG_TRANSCODE_ONLY |
        MFT_ENUM_FLAG_HARDWARE |
        MFT_ENUM_FLAG_SORTANDFILTER;

    constexpr UINT32 kHardwareEncoderFlags =
        MFT_ENUM_FLAG_HARDWARE |
        MFT_ENUM_FLAG_SORTANDFILTER;

    EnumerateMediaFoundationEncoders(result, kAllEncoderFlags, false);
    EnumerateMediaFoundationEncoders(result, kHardwareEncoderFlags, true);

    MFShutdown();
    return result;
}

D3D11MediaFoundationCapabilities D3D11VideoEncoder::QueryMediaFoundationCapabilities() {
    D3D11MediaFoundationCapabilities caps;
    caps.h264Nv12 = QueryMediaFoundationSupport(VideoCodec::H264, VideoPixelFormat::NV12);
    caps.hevcNv12 = QueryMediaFoundationSupport(VideoCodec::HEVC, VideoPixelFormat::NV12);
    caps.hevcP010 = QueryMediaFoundationSupport(VideoCodec::HEVC, VideoPixelFormat::P010);
    caps.av1Nv12 = QueryMediaFoundationSupport(VideoCodec::AV1, VideoPixelFormat::NV12);
    caps.av1P010 = QueryMediaFoundationSupport(VideoCodec::AV1, VideoPixelFormat::P010);
    return caps;
}

} // namespace D3DVideoEncoderLib
