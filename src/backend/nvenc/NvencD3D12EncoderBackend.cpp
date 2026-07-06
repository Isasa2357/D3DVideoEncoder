#include "backend/nvenc/NvencD3D12EncoderBackend.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

#include <D3D12Helper/D3D12Core/D3D12Barrier.hpp>
#include <D3D12Helper/D3D12Framework/D3D12Helpers.hpp>

#include <wrl/client.h>
#include <exception>
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

bool RectEqualsFull(const D3D12CoreLib::Processing::ProcessingRect& r, UINT width, UINT height) noexcept {
    return r.x == 0 && r.y == 0 && r.width == width && r.height == height;
}
} // namespace

NvencD3D12EncoderBackend::NvencD3D12EncoderBackend(DebugLog log) : log_(log) {}
NvencD3D12EncoderBackend::~NvencD3D12EncoderBackend() {
    try { close(); } catch (...) {}
}

bool NvencD3D12EncoderBackend::inputAlreadyMatchesInternalFormat() const noexcept {
    return desc_.input.inputFormat == ToDxgiFormat(desc_.internalFormat);
}

uint32_t NvencD3D12EncoderBackend::sourceWidth() const noexcept {
    return desc_.input.sourceWidth != 0 ? desc_.input.sourceWidth : desc_.width;
}

uint32_t NvencD3D12EncoderBackend::sourceHeight() const noexcept {
    return desc_.input.sourceHeight != 0 ? desc_.input.sourceHeight : desc_.height;
}

D3D12CoreLib::Processing::ProcessingRect NvencD3D12EncoderBackend::resolvedSourceRect() const {
    const UINT srcW = sourceWidth();
    const UINT srcH = sourceHeight();
    if (srcW == 0 || srcH == 0) {
        throw D3DVideoEncoderError("NvencD3D12 sourceWidth/sourceHeight must resolve to non-zero values.");
    }

    if (desc_.input.sourceRect.isEmpty()) {
        return { 0, 0, srcW, srcH };
    }

    const auto& r = desc_.input.sourceRect;
    if (r.x < 0 || r.y < 0 || r.width == 0 || r.height == 0 ||
        static_cast<uint64_t>(r.x) + r.width > srcW ||
        static_cast<uint64_t>(r.y) + r.height > srcH) {
        std::ostringstream oss;
        oss << "NvencD3D12 sourceRect is outside the source texture. source="
            << srcW << "x" << srcH
            << " rect=(" << r.x << "," << r.y << "," << r.width << "," << r.height << ")";
        throw D3DVideoEncoderError(oss.str());
    }
    return { r.x, r.y, r.width, r.height };
}

bool NvencD3D12EncoderBackend::needsResizeOrCrop() const {
    const auto rect = resolvedSourceRect();
    return !RectEqualsFull(rect, sourceWidth(), sourceHeight()) ||
           rect.width != desc_.width ||
           rect.height != desc_.height;
}

bool NvencD3D12EncoderBackend::inputIsRgbaLike() const noexcept {
    return D3D12CoreLib::Processing::IsRgbaLikeFormat(desc_.input.inputFormat);
}

D3D12CoreLib::Processing::ProcessingFilter NvencD3D12EncoderBackend::processingFilter() const noexcept {
    switch (desc_.input.resizeFilter) {
    case VideoProcessingFilter::Point: return D3D12CoreLib::Processing::ProcessingFilter::Point;
    case VideoProcessingFilter::Linear: return D3D12CoreLib::Processing::ProcessingFilter::Linear;
    default: return D3D12CoreLib::Processing::ProcessingFilter::Linear;
    }
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
    if (sourceWidth() == 0 || sourceHeight() == 0) {
        throw D3DVideoEncoderError("NvencD3D12 sourceWidth/sourceHeight resolved to zero.");
    }

    const bool resizeOrCrop = needsResizeOrCrop();
    useProcessing_ = !inputAlreadyMatchesInternalFormat() || resizeOrCrop;
    if (useProcessing_ && !desc_.input.allowFormatConversion) {
        throw D3DVideoEncoderError(
            "NvencD3D12 requires D3D12Processing because the input format/size/crop does not directly match the encode surface, "
            "but desc.input.allowFormatConversion=false.");
    }
    if (resizeOrCrop && !inputIsRgbaLike()) {
        throw D3DVideoEncoderError(
            "NvencD3D12 crop/resize currently requires RGB-like D3D12 input. Direct NV12/P010 crop/resize is not supported by the current D3D12Processing path.");
    }

    directStateCommandContext_ = desc_.input.core->CreateDirectContext();

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
        ? "NvencD3D12EncoderBackend initialize with D3D12Processing conversion/crop/resize"
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
    resizer_.Initialize(processingContext_);
    fusedProcessor_.Initialize(processingContext_);
    commandContext_ = core.CreateDirectContext();

    if (needsResizeOrCrop()) {
        resizedTexture_ = resizer_.CreateOutputTexture(
            core,
            desc_.width,
            desc_.height,
            desc_.input.inputFormat,
            D3D12_RESOURCE_STATE_COMMON);
        resizedTextureState_ = D3D12_RESOURCE_STATE_COMMON;
    }

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
        rd.Width != sourceWidth() ||
        rd.Height != sourceHeight() ||
        rd.Format != desc_.input.inputFormat) {
        std::ostringstream oss;
        oss << "NvencD3D12 input resource mismatch. expected="
            << sourceWidth() << "x" << sourceHeight()
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

void NvencD3D12EncoderBackend::validateDirectNvencResource(ID3D12Resource* resource) const {
    validateInputResource(resource);
    if (!inputAlreadyMatchesInternalFormat()) {
        throw D3DVideoEncoderError("NvencD3D12 direct path requires inputFormat to match internalFormat.");
    }
}

void NvencD3D12EncoderBackend::transitionResourceBlocking(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {

    if (!resource || before == after) return;

    auto& core = *desc_.input.core;
    if (directStateFenceValue_ != 0) {
        core.DirectQueue().WaitForFenceValue(directStateFenceValue_);
        directStateFenceValue_ = 0;
    }

    directStateCommandContext_.Reset();
    const auto barrier = D3D12CoreLib::MakeTransitionBarrier(resource, before, after);
    directStateCommandContext_.ResourceBarrier(barrier);
    directStateCommandContext_.Close();

    ID3D12CommandList* lists[] = { directStateCommandContext_.GetCommandList() };
    core.DirectQueue().ExecuteCommandLists(1, lists);
    directStateFenceValue_ = core.DirectQueue().Signal();
    core.DirectQueue().WaitForFenceValue(directStateFenceValue_);
    directStateFenceValue_ = 0;
}

ID3D12Resource* NvencD3D12EncoderBackend::prepareDirectNvencResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState) {
    validateDirectNvencResource(resource);
    transitionResourceBlocking(resource, currentState, D3D12_RESOURCE_STATE_COMMON);
    return resource;
}

void NvencD3D12EncoderBackend::restoreDirectNvencResource(ID3D12Resource* resource, D3D12_RESOURCE_STATES originalState) {
    if (desc_.input.restoreStateAfterEncode) {
        transitionResourceBlocking(resource, D3D12_RESOURCE_STATE_COMMON, originalState);
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

    const auto srcRect = resolvedSourceRect();
    D3D12CoreLib::D3D12Resource* convertSource = &src;
    D3D12_RESOURCE_STATES convertSourceState = currentState;

    commandContext_.Reset();

    if (needsResizeOrCrop()) {
        D3D12CoreLib::Processing::D3D12ProcessingStateDesc resizeStates = {};
        resizeStates.useExplicitStates = true;
        resizeStates.srcBefore = currentState;
        resizeStates.srcAfter = currentState;
        resizeStates.dstBefore = resizedTextureState_;
        resizeStates.dstAfter = D3D12_RESOURCE_STATE_COMMON;

        if (desc_.input.preferFusedResize) {
            D3D12CoreLib::Processing::FusedConvertResizeDesc fused = {};
            fused.srcFormat = desc_.input.inputFormat;
            fused.dstFormat = desc_.input.inputFormat;
            fused.color.srcMatrix = ToProcessingMatrix(desc_.colorMatrix);
            fused.color.dstMatrix = ToProcessingMatrix(desc_.colorMatrix);
            fused.color.srcRange = ToProcessingRange(desc_.colorRange);
            fused.color.dstRange = ToProcessingRange(desc_.colorRange);
            fused.color.alphaMode = D3D12CoreLib::Processing::ProcessingAlphaMode::Ignore;
            fused.filter = processingFilter();
            fused.srcRect = srcRect;
            fused.dstRect = { 0, 0, desc_.width, desc_.height };
            fusedProcessor_.RecordConvertResize(commandContext_, src, resizedTexture_, fused, resizeStates);
        } else {
            D3D12CoreLib::Processing::ResizeDesc resize = {};
            resize.filter = processingFilter();
            resize.srcRect = srcRect;
            resize.dstRect = { 0, 0, desc_.width, desc_.height };
            resizer_.RecordResize(commandContext_, src, resizedTexture_, resize, resizeStates);
        }

        resizedTextureState_ = D3D12_RESOURCE_STATE_COMMON;
        convertSource = &resizedTexture_;
        convertSourceState = D3D12_RESOURCE_STATE_COMMON;
    }

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

    D3D12CoreLib::Processing::D3D12ProcessingStateDesc convertStates = {};
    convertStates.useExplicitStates = true;
    convertStates.srcBefore = convertSourceState;
    convertStates.srcAfter = convertSourceState;
    convertStates.dstBefore = convertedTextureState_;
    convertStates.dstAfter = D3D12_RESOURCE_STATE_COMMON;

    formatConverter_.RecordConvert(commandContext_, *convertSource, convertedTexture_, convert, convertStates);
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
        session_.encodeDirectXResource(nvencInput, timestamp100ns, duration100ns);
        return;
    }

    nvencInput = prepareDirectNvencResource(resource, currentState);
    std::exception_ptr pending = nullptr;
    try {
        session_.encodeDirectXResource(nvencInput, timestamp100ns, duration100ns);
    } catch (...) {
        pending = std::current_exception();
    }

    try {
        restoreDirectNvencResource(resource, currentState);
    } catch (...) {
        if (!pending) pending = std::current_exception();
    }
    if (pending) {
        std::rethrow_exception(pending);
    }
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
    if (directStateFenceValue_ != 0 && desc_.input.core) {
        desc_.input.core->DirectQueue().WaitForFenceValue(directStateFenceValue_);
        directStateFenceValue_ = 0;
    }
    session_.close();
    open_ = false;
    log_.info("NvencD3D12EncoderBackend closed");
}

} // namespace D3DVideoEncoderLib
