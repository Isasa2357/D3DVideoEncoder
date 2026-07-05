#include "backend/MediaFoundationEncoderBackend.hpp"

#include <D3DVideoEncoder/D3D11VideoEncoder.hpp>

#include "util/HResult.hpp"

#include <codecapi.h>
#include <mferror.h>
#include <mfobjects.h>

#include <sstream>
#include <string>

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

GUID MediaFoundationEncoderBackend::inputSubtype() const {
    switch (desc_.internalFormat) {
    case VideoPixelFormat::NV12: return MFVideoFormat_NV12;
    case VideoPixelFormat::P010: return MFVideoFormat_P010;
    default:
        throw D3DVideoEncoderError("Unsupported Media Foundation input pixel format.");
    }
}

std::string MediaFoundationEncoderBackend::describeConfiguration() const {
    std::ostringstream oss;
    oss << "MediaFoundation backend"
        << " codec=" << ToString(desc_.codec)
        << " input=" << ToString(desc_.internalFormat)
        << " size=" << desc_.width << "x" << desc_.height
        << " fps=" << desc_.frameRateNum << "/" << desc_.frameRateDen
        << " bitrate=" << desc_.bitrate
        << " hardwareTransforms=" << (desc_.enableHardwareTransform ? "on" : "off")
        << " hardwareOnly=" << (desc_.useOnlyHardwareTransform ? "on" : "off");
    return oss.str();
}

void MediaFoundationEncoderBackend::verifyCapabilities() const {
    const auto caps = D3D11VideoEncoder::QueryMediaFoundationSupport(desc_.codec, desc_.internalFormat);

    if (!caps.supported) {
        std::ostringstream oss;
        oss << "No Media Foundation video encoder MFT was found for "
            << "codec=" << ToString(desc_.codec)
            << ", inputFormat=" << ToString(desc_.internalFormat)
            << ". queryHr=" << HResultToString(caps.queryHr)
            << ". H.264 requires NV12. HEVC/P010 support depends on the Windows installation, GPU driver, "
            << "and optional HEVC components.";
        throw D3DVideoEncoderError(oss.str());
    }

    if (desc_.useOnlyHardwareTransform && !caps.hardwareSupported) {
        std::ostringstream oss;
        oss << "desc.useOnlyHardwareTransform is true, but no hardware Media Foundation encoder "
            << "was found for codec=" << ToString(desc_.codec)
            << ", inputFormat=" << ToString(desc_.internalFormat)
            << ". hardwareQueryHr=" << HResultToString(caps.hardwareQueryHr)
            << ". Set useOnlyHardwareTransform=false or choose another codec/format.";
        throw D3DVideoEncoderError(oss.str());
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

    verifyCapabilities();

    runtime_.startup();

    log_.info("Media Foundation runtime started");

    D3DVE_THROW_IF_FAILED_MSG(MFCreateDXGIDeviceManager(&resetToken_, &dxgiDeviceManager_), describeConfiguration() + ": MFCreateDXGIDeviceManager");
    D3DVE_THROW_IF_FAILED_MSG(dxgiDeviceManager_->ResetDevice(mfCore->GetDevice(), resetToken_), describeConfiguration() + ": IMFDXGIDeviceManager::ResetDevice");

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    D3DVE_THROW_IF_FAILED_MSG(MFCreateAttributes(&attributes, 4), describeConfiguration() + ": MFCreateAttributes for sink writer");
    D3DVE_THROW_IF_FAILED_MSG(attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, dxgiDeviceManager_.Get()), describeConfiguration() + ": set MF_SINK_WRITER_D3D_MANAGER");
    D3DVE_THROW_IF_FAILED_MSG(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, desc_.enableHardwareTransform ? TRUE : FALSE), describeConfiguration() + ": set MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS");

    D3DVE_THROW_IF_FAILED_MSG(MFCreateSinkWriterFromURL(
        desc_.outputPath.c_str(),
        nullptr,
        attributes.Get(),
        &sinkWriter_),
        describeConfiguration() + ": MFCreateSinkWriterFromURL outputPath=" + std::string(desc_.outputPath.begin(), desc_.outputPath.end()));

    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    D3DVE_THROW_IF_FAILED_MSG(MFCreateMediaType(&outputType), describeConfiguration() + ": MFCreateMediaType output");
    configureOutputType(outputType.Get());
    D3DVE_THROW_IF_FAILED_MSG(sinkWriter_->AddStream(outputType.Get(), &streamIndex_), describeConfiguration() + ": IMFSinkWriter::AddStream");

    Microsoft::WRL::ComPtr<IMFMediaType> inputType;
    D3DVE_THROW_IF_FAILED_MSG(MFCreateMediaType(&inputType), describeConfiguration() + ": MFCreateMediaType input");
    configureInputType(inputType.Get());
    D3DVE_THROW_IF_FAILED_MSG(sinkWriter_->SetInputMediaType(streamIndex_, inputType.Get(), nullptr), describeConfiguration() + ": IMFSinkWriter::SetInputMediaType");

    D3DVE_THROW_IF_FAILED_MSG(sinkWriter_->BeginWriting(), describeConfiguration() + ": IMFSinkWriter::BeginWriting");
    open_ = true;

    log_.info("MediaFoundationEncoderBackend BeginWriting succeeded");
}

void MediaFoundationEncoderBackend::configureOutputType(IMFMediaType* mediaType) const {
    const std::string context = describeConfiguration() + ": configure output type";
    D3DVE_THROW_IF_FAILED_MSG(mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), context + ": MF_MT_MAJOR_TYPE");
    D3DVE_THROW_IF_FAILED_MSG(mediaType->SetGUID(MF_MT_SUBTYPE, codecSubtype()), context + ": MF_MT_SUBTYPE");
    D3DVE_THROW_IF_FAILED_MSG(mediaType->SetUINT32(MF_MT_AVG_BITRATE, desc_.bitrate), context + ": MF_MT_AVG_BITRATE");
    D3DVE_THROW_IF_FAILED_MSG(mediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), context + ": MF_MT_INTERLACE_MODE");
    D3DVE_THROW_IF_FAILED_MSG(MFSetAttributeSize(mediaType, MF_MT_FRAME_SIZE, desc_.width, desc_.height), context + ": MF_MT_FRAME_SIZE");
    D3DVE_THROW_IF_FAILED_MSG(MFSetAttributeRatio(mediaType, MF_MT_FRAME_RATE, desc_.frameRateNum, desc_.frameRateDen), context + ": MF_MT_FRAME_RATE");
    D3DVE_THROW_IF_FAILED_MSG(MFSetAttributeRatio(mediaType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1), context + ": MF_MT_PIXEL_ASPECT_RATIO");

    if (desc_.codec == VideoCodec::H264) {
        // High profile is a good default for high-quality recording while remaining broadly compatible.
        mediaType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    }
}

void MediaFoundationEncoderBackend::configureInputType(IMFMediaType* mediaType) const {
    const std::string context = describeConfiguration() + ": configure input type";
    D3DVE_THROW_IF_FAILED_MSG(mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), context + ": MF_MT_MAJOR_TYPE");
    D3DVE_THROW_IF_FAILED_MSG(mediaType->SetGUID(MF_MT_SUBTYPE, inputSubtype()), context + ": MF_MT_SUBTYPE");
    D3DVE_THROW_IF_FAILED_MSG(mediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), context + ": MF_MT_INTERLACE_MODE");
    D3DVE_THROW_IF_FAILED_MSG(MFSetAttributeSize(mediaType, MF_MT_FRAME_SIZE, desc_.width, desc_.height), context + ": MF_MT_FRAME_SIZE");
    D3DVE_THROW_IF_FAILED_MSG(MFSetAttributeRatio(mediaType, MF_MT_FRAME_RATE, desc_.frameRateNum, desc_.frameRateDen), context + ": MF_MT_FRAME_RATE");
    D3DVE_THROW_IF_FAILED_MSG(MFSetAttributeRatio(mediaType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1), context + ": MF_MT_PIXEL_ASPECT_RATIO");
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
    D3DVE_THROW_IF_FAILED_MSG(MFCreateDXGISurfaceBuffer(
        __uuidof(ID3D11Texture2D),
        surface.d3d11Texture.Get(),
        surface.subresource,
        FALSE,
        &buffer),
        describeConfiguration() + ": MFCreateDXGISurfaceBuffer");

    DWORD maxLength = 0;
    if (SUCCEEDED(buffer->GetMaxLength(&maxLength)) && maxLength > 0) {
        D3DVE_THROW_IF_FAILED_MSG(buffer->SetCurrentLength(maxLength), describeConfiguration() + ": IMFMediaBuffer::SetCurrentLength");
    }

    Microsoft::WRL::ComPtr<IMFSample> sample;
    D3DVE_THROW_IF_FAILED_MSG(MFCreateSample(&sample), describeConfiguration() + ": MFCreateSample");
    D3DVE_THROW_IF_FAILED_MSG(sample->AddBuffer(buffer.Get()), describeConfiguration() + ": IMFSample::AddBuffer");
    D3DVE_THROW_IF_FAILED_MSG(sample->SetSampleTime(timestamp100ns), describeConfiguration() + ": IMFSample::SetSampleTime");
    D3DVE_THROW_IF_FAILED_MSG(sample->SetSampleDuration(duration100ns), describeConfiguration() + ": IMFSample::SetSampleDuration");

    D3DVE_THROW_IF_FAILED_MSG(sinkWriter_->WriteSample(streamIndex_, sample.Get()), describeConfiguration() + ": IMFSinkWriter::WriteSample");
}

void MediaFoundationEncoderBackend::flush() {
    if (!open_ || !sinkWriter_) return;
    D3DVE_THROW_IF_FAILED_MSG(sinkWriter_->Flush(streamIndex_), describeConfiguration() + ": IMFSinkWriter::Flush");
}

void MediaFoundationEncoderBackend::close() {
    if (!open_) return;

    log_.info("MediaFoundationEncoderBackend Finalize");
    HRESULT hr = sinkWriter_->Finalize();
    open_ = false;
    sinkWriter_.Reset();
    dxgiDeviceManager_.Reset();

    D3DVE_THROW_IF_FAILED_MSG(hr, describeConfiguration() + ": IMFSinkWriter::Finalize");
}

} // namespace D3DVideoEncoderLib
