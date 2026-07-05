#include "backend/nvenc/NvencD3D12EncoderBackend.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

#include <D3D12Helper/D3D12Framework/D3D12Helpers.hpp>

#include <wrl/client.h>
#include <sstream>
#include <utility>

namespace D3DVideoEncoderLib {
namespace {
D3D12CoreLib::Processing::ProcessingColorMatrix ToProcessingMatrix(VideoColorMatrix matrix) noexcept {
    switch (matrix) {
    case VideoColorMatrix::BT601: return D3D12CoreLib::Processing::ProcessingColorMatrix::BT601;
    case VideoColorMatrix::BT709: return D3D12CoreLib::Processing::ProcessingColorMatrix::BT709;
    case VideoColorMatrix::BT2020: return D3D12CoreLib::Processing::ProcessingColorMatrix::BT2020;
    default: return D3D12CoreLib::Processing::ProcessingColorMatrix::BT709;
    }
}

D3D12CoreLib::Processing::ProcessingColorRange ToProcessingRange(VideoColorRange range) noexcept {
    switch (range) {
    case VideoColorRange::Full: return D3D12CoreLib::Processing::ProcessingColorRange::Full;
    case VideoColorRange::Limited: return D3D12CoreLib::Processing::ProcessingColorRange::Limited;
    default: return D3D12CoreLib::Processing::ProcessingColorRange::Limited;
    }
}

std::string FormatDxgi(DXGI_FORMAT format) {
    std::ostringstream oss;
    oss << static_cast<int>(format);
    return oss.str();
}
} // namespace

NvencD3D12EncoderBackend::NvencD3D12EncoderBackend(DebugLog log) : log_(log) {}
NvencD3D12EncoderBackend::~NvencD3D12EncoderBackend() {
    try { close(); } catch (...) {}
}

bool NvencD3D12EncoderBackend::inputAlreadyMatchesInternalFormat() const noexcept {
    return desc_.input.inputFormat == ToDxgiFormat(desc_.internalFormat);
}

void NvencD3D12EncoderBackend::initialize(const D3D12VideoEncoderDesc& desc) {
    desc_ = desc;
    if (!desc_.input.core) {
        throw D3DVideoEncoderError("NvencD3D12EncoderBackend requires desc.input.core.");
    }
    if (!IsYuv420EncodeFormat(desc_.internalFormat)) {
        throw D3DVideoEncoderError("NvencD3D12EncoderBackend requires internalFormat NV12 or P010.");
    }
    if (desc_.codec == VideoCodec::H264 && desc_.internalFormat != VideoPixelFormat::NV12) {
        throw D3DVideoEncoderError("NvencD3D12 H.264 requires internalFormat=NV12.");
    }
    if (!IsSupportedD3D12InputFormat(desc_.input.inputFormat)) {
        throw D3DVideoEncoderError("NvencD3D12 input.inputFormat is unsupported.");
    }

    useProcessing_ = !inputAlreadyMatchesInternalFormat();
    if (useProcessing_ && !desc_.input.allowFormatConversion) {
        throw D3DVideoEncoderError(
            "NvencD3D12 input format does not match internalFormat and desc.input.allowFormatConversion=false.");
    }

    if (useProcessing_) {
        initializeProcessingIfNeeded();
    }

    NvencSessionDesc sessionDesc = {};
    sessionDesc.outputPath = desc_.outputPath;
    sessionDesc.width = desc_.width;
    sessionDesc.height = desc_.height;
    sessionDesc.frameRateNum = desc_.frameRateNum;
    sessionDesc.frameRateDen = desc_.frameRateDen;
    sessionDesc.bitrate = desc_.bitrate;
    sessionDesc.gopLength = desc_.gopLength;
    sessionDesc.bFrameCount = desc_.bFrameCount;
    sessionDesc.codec = desc_.codec;
    sessionDesc.inputFormat = desc_.internalFormat;
    sessionDesc.rateControl = desc_.rateControl;

    log_.info(useProcessing_
        ? "NvencD3D12EncoderBackend initialize with D3D12Processing conversion"
        : "NvencD3D12EncoderBackend initialize with direct NV12/P010 input");
    session_.initialize(desc_.input.core->GetDevice(), NV_ENC_DEVICE_TYPE_DIRECTX, sessionDesc);
    open_ = true;
}

void NvencD3D12EncoderBackend::initializeProcessingIfNeeded() {
    auto& core = *desc_.input.core;
    cbvSrvUavAllocator_.Initialize(core.GetDevice(),
                                   D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                   desc_.input.cbvSrvUavDescriptorCount,
                                   true);
    samplerAllocator_.Initialize(core.GetDevice(),
                                 D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
                                 desc_.input.samplerDescriptorCount,
                                 true);
    processingContext_.Initialize(core,
                                  &cbvSrvUavAllocator_,
                                  &samplerAllocator_,
                                  desc_.input.processingShaderDirectory);
    formatConverter_.Initialize(processingContext_);
    commandContext_ = core.CreateDirectContext();
    convertedTexture_ = formatConverter_.CreateOutputTexture(
        core,
        desc_.width,
        desc_.height,
        ToDxgiFormat(desc_.internalFormat),
        D3D12_RESOURCE_STATE_COMMON);
    convertedTextureState_ = D3D12_RESOURCE_STATE_COMMON;
}

void NvencD3D12EncoderBackend::validateInputResource(ID3D12Resource* resource) const {
    if (!resource) {
        throw D3DVideoEncoderError("NvencD3D12 encode received a null ID3D12Resource.");
    }

    const D3D12_RESOURCE_DESC rd = resource->GetDesc();
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        rd.Width != desc_.width ||
        rd.Height != desc_.height ||
        rd.Format != desc_.input.inputFormat) {
        std::ostringstream oss;
        oss << "NvencD3D12 input resource mismatch. expected="
            << desc_.width << "x" << desc_.height
            << " inputFormat=" << FormatDxgi(desc_.input.inputFormat)
            << " actual=" << static_cast<uint64_t>(rd.Width) << "x" << rd.Height
            << " format=" << FormatDxgi(rd.Format);
        throw D3DVideoEncoderError(oss.str());
    }

    Microsoft::WRL::ComPtr<ID3D12Device> resourceDevice;
    resource->GetDevice(IID_PPV_ARGS(&resourceDevice));
    if (resourceDevice.Get() != desc_.input.core->GetDevice()) {
        throw D3DVideoEncoderError("NvencD3D12 input resource belongs to a different ID3D12Device than desc.input.core.");
    }
}

void NvencD3D12EncoderBackend::validateDirectNvencResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState) const {
    validateInputResource(resource);
    if (currentState != D3D12_RESOURCE_STATE_COMMON) {
        throw D3DVideoEncoderError(
            "NvencD3D12 direct NV12/P010 path requires the input resource state to be D3D12_RESOURCE_STATE_COMMON. "
            "Use RGB input with D3D12Processing conversion, or transition the resource to COMMON before write().");
    }
}

ID3D12Resource* NvencD3D12EncoderBackend::convertToInternalFormat(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState) {
    validateInputResource(resource);

    auto& core = *desc_.input.core;
    if (processingFenceValue_ != 0) {
        core.DirectQueue().WaitForFenceValue(processingFenceValue_);
        processingFenceValue_ = 0;
    }

    cbvSrvUavAllocator_.Reset();
    samplerAllocator_.Reset();

    Microsoft::WRL::ComPtr<ID3D12Resource> srcComPtr;
    srcComPtr = resource;
    D3D12CoreLib::D3D12Resource src(std::move(srcComPtr), currentState);

    D3D12CoreLib::Processing::FormatConvertDesc convert = {};
    convert.srcFormat = desc_.input.inputFormat;
    convert.dstFormat = ToDxgiFormat(desc_.internalFormat);
    convert.color.srcMatrix = ToProcessingMatrix(desc_.colorMatrix);
    convert.color.dstMatrix = ToProcessingMatrix(desc_.colorMatrix);
    convert.color.srcRange = ToProcessingRange(desc_.colorRange);
    convert.color.dstRange = ToProcessingRange(desc_.colorRange);
    convert.color.alphaMode = D3D12CoreLib::Processing::ProcessingAlphaMode::Ignore;
    convert.srcRect = { 0, 0, desc_.width, desc_.height };
    convert.dstRect = { 0, 0, desc_.width, desc_.height };

    D3D12CoreLib::Processing::D3D12ProcessingStateDesc states = {};
    states.useExplicitStates = true;
    states.srcBefore = currentState;
    states.srcAfter = currentState;
    states.dstBefore = convertedTextureState_;
    states.dstAfter = D3D12_RESOURCE_STATE_COMMON;

    commandContext_.Reset();
    formatConverter_.RecordConvert(commandContext_, src, convertedTexture_, convert, states);
    commandContext_.Close();

    ID3D12CommandList* lists[] = { commandContext_.GetCommandList() };
    core.DirectQueue().ExecuteCommandLists(1, lists);
    processingFenceValue_ = core.DirectQueue().Signal();
    core.DirectQueue().WaitForFenceValue(processingFenceValue_);
    processingFenceValue_ = 0;
    convertedTextureState_ = D3D12_RESOURCE_STATE_COMMON;

    return convertedTexture_.Get();
}

void NvencD3D12EncoderBackend::encode(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns, int64_t duration100ns) {
    if (!open_) {
        throw D3DVideoEncoderError("NvencD3D12 encode called after close.");
    }

    ID3D12Resource* nvencInput = resource;
    if (useProcessing_) {
        nvencInput = convertToInternalFormat(resource, currentState);
    } else {
        validateDirectNvencResource(resource, currentState);
    }

    session_.encodeDirectXResource(nvencInput, timestamp100ns, duration100ns);
}

void NvencD3D12EncoderBackend::flush() {
    if (open_) {
        session_.flush();
    }
}

void NvencD3D12EncoderBackend::close() {
    if (!open_) return;
    if (processingFenceValue_ != 0 && desc_.input.core) {
        desc_.input.core->DirectQueue().WaitForFenceValue(processingFenceValue_);
        processingFenceValue_ = 0;
    }
    session_.close();
    open_ = false;
    log_.info("NvencD3D12EncoderBackend closed");
}

} // namespace D3DVideoEncoderLib
