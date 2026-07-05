#include "backend/MediaFoundationEncoderBackend.hpp"

#include "util/HResult.hpp"

#include <codecapi.h>
#include <mferror.h>
#include <mfobjects.h>

#include <sstream>

namespace D3DVideoEncoderLib {

MediaFoundationEncoderBackend::MediaFoundationEncoderBackend(DebugLog log)
    : log_(log) {}

MediaFoundationEncoderBackend::~MediaFoundationEncoderBackend() {
    try {
        close();
    } catch (...) {
    }
}

GUID MediaFoundationEncoderBackend::codecSubtype() const {
    switch (desc_.codec) {
    case VideoCodec::H264: return MFVideoFormat_H264;
    case VideoCodec::HEVC: return MFVideoFormat_HEVC;
    case VideoCodec::AV1:
#ifdef MFVideoFormat_AV1
        return MFVideoFormat_AV1;
#else
        throw D3DVideoEncoderError("MFVideoFormat_AV1 is not available in this Windows SDK.");
#endif
    default:
        throw D3DVideoEncoderError("Unsupported Media Foundation codec.");
    }
}

void MediaFoundationEncoderBackend::initialize(const D3D11VideoEncoderDesc& desc) {
    desc_ = desc;

    D3D11CoreLib::D3D11Core* mfCore = desc_.input.core;
    if (!mfCore) {
        throw D3DVideoEncoderError("MediaFoundation backend requires desc.input.core.");
    }
    if (desc_.codec == VideoCodec::AV1) {
        throw D3DVideoEncoderError("Media Foundation AV1 encode support is not enabled in this implementation; use NVENC AV1 when available.");
    }
    if (!IsYuv420EncodeFormat(desc_.internalFormat)) {
        throw D3DVideoEncoderError("Media Foundation backend requires internalFormat NV12 or P010.");
    }
    if (desc_.codec == VideoCodec::H264 && desc_.internalFormat != VideoPixelFormat::NV12) {
        throw D3DVideoEncoderError("Media Foundation H.264 path requires NV12. Use HEVC for P010.");
    }
    if (desc_.outputPath.empty()) {
        throw D3DVideoEncoderError("outputPath is empty.");
    }

    runtime_.startup();

    log_.info("Media Foundation runtime started");

    D3DVE_THROW_IF_FAILED(MFCreateDXGIDeviceManager(&resetToken_, &dxgiDeviceManager_));
    D3DVE_THROW_IF_FAILED(dxgiDeviceManager_->ResetDevice(mfCore->GetDevice(), resetToken_));

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    D3DVE_THROW_IF_FAILED(MFCreateAttributes(&attributes, 4));
    D3DVE_THROW_IF_FAILED(attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, dxgiDeviceManager_.Get()));
    D3DVE_THROW_IF_FAILED(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, desc_.enableHardwareTransform ? TRUE : FALSE));

    D3DVE_THROW_IF_FAILED(MFCreateSinkWriterFromURL(
        desc_.outputPath.c_str(),
        nullptr,
        attributes.Get(),
        &sinkWriter_));

    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    D3DVE_THROW_IF_FAILED(MFCreateMediaType(&outputType));
    configureOutputType(outputType.Get());
    D3DVE_THROW_IF_FAILED(sinkWriter_->AddStream(outputType.Get(), &streamIndex_));

    Microsoft::WRL::ComPtr<IMFMediaType> inputType;
    D3DVE_THROW_IF_FAILED(MFCreateMediaType(&inputType));
    configureInputType(inputType.Get());
    D3DVE_THROW_IF_FAILED(sinkWriter_->SetInputMediaType(streamIndex_, inputType.Get(), nullptr));

    D3DVE_THROW_IF_FAILED(sinkWriter_->BeginWriting());
    open_ = true;

    log_.info("MediaFoundationEncoderBackend BeginWriting succeeded");
}

void MediaFoundationEncoderBackend::configureOutputType(IMFMediaType* mediaType) const {
    D3DVE_THROW_IF_FAILED(mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    D3DVE_THROW_IF_FAILED(mediaType->SetGUID(MF_MT_SUBTYPE, codecSubtype()));
    D3DVE_THROW_IF_FAILED(mediaType->SetUINT32(MF_MT_AVG_BITRATE, desc_.bitrate));
    D3DVE_THROW_IF_FAILED(mediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    D3DVE_THROW_IF_FAILED(MFSetAttributeSize(mediaType, MF_MT_FRAME_SIZE, desc_.width, desc_.height));
    D3DVE_THROW_IF_FAILED(MFSetAttributeRatio(mediaType, MF_MT_FRAME_RATE, desc_.frameRateNum, desc_.frameRateDen));
    D3DVE_THROW_IF_FAILED(MFSetAttributeRatio(mediaType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));

    if (desc_.codec == VideoCodec::H264) {
        // High profile is a good default for high-quality recording while remaining broadly compatible.
        mediaType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    }
}

void MediaFoundationEncoderBackend::configureInputType(IMFMediaType* mediaType) const {
    D3DVE_THROW_IF_FAILED(mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    D3DVE_THROW_IF_FAILED(mediaType->SetGUID(MF_MT_SUBTYPE, desc_.internalFormat == VideoPixelFormat::P010 ? MFVideoFormat_P010 : MFVideoFormat_NV12));
    D3DVE_THROW_IF_FAILED(mediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    D3DVE_THROW_IF_FAILED(MFSetAttributeSize(mediaType, MF_MT_FRAME_SIZE, desc_.width, desc_.height));
    D3DVE_THROW_IF_FAILED(MFSetAttributeRatio(mediaType, MF_MT_FRAME_RATE, desc_.frameRateNum, desc_.frameRateDen));
    D3DVE_THROW_IF_FAILED(MFSetAttributeRatio(mediaType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
}

void MediaFoundationEncoderBackend::encode(
    const EncodeSurface& surface,
    int64_t timestamp100ns,
    int64_t duration100ns) {

    if (!open_ || !sinkWriter_) {
        throw D3DVideoEncoderError("MediaFoundationEncoderBackend is not open.");
    }
    if (surface.api != EncodeSurface::Api::D3D11 || !surface.d3d11Texture) {
        throw D3DVideoEncoderError("MediaFoundationEncoderBackend requires a D3D11 encode surface.");
    }
    if (surface.format != ToDxgiFormat(desc_.internalFormat)) {
        throw D3DVideoEncoderError("MediaFoundationEncoderBackend input surface format does not match desc.internalFormat.");
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    D3DVE_THROW_IF_FAILED(MFCreateDXGISurfaceBuffer(
        __uuidof(ID3D11Texture2D),
        surface.d3d11Texture.Get(),
        surface.subresource,
        FALSE,
        &buffer));

    DWORD maxLength = 0;
    if (SUCCEEDED(buffer->GetMaxLength(&maxLength)) && maxLength > 0) {
        buffer->SetCurrentLength(maxLength);
    }

    Microsoft::WRL::ComPtr<IMFSample> sample;
    D3DVE_THROW_IF_FAILED(MFCreateSample(&sample));
    D3DVE_THROW_IF_FAILED(sample->AddBuffer(buffer.Get()));
    D3DVE_THROW_IF_FAILED(sample->SetSampleTime(timestamp100ns));
    D3DVE_THROW_IF_FAILED(sample->SetSampleDuration(duration100ns));

    D3DVE_THROW_IF_FAILED(sinkWriter_->WriteSample(streamIndex_, sample.Get()));
}

void MediaFoundationEncoderBackend::flush() {
    if (!open_ || !sinkWriter_) return;
    D3DVE_THROW_IF_FAILED(sinkWriter_->Flush(streamIndex_));
}

void MediaFoundationEncoderBackend::close() {
    if (!open_) return;

    log_.info("MediaFoundationEncoderBackend Finalize");
    HRESULT hr = sinkWriter_->Finalize();
    open_ = false;
    sinkWriter_.Reset();
    dxgiDeviceManager_.Reset();

    D3DVE_THROW_IF_FAILED(hr);
}

} // namespace D3DVideoEncoderLib
