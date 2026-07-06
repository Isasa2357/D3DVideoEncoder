#include "backend/d3d12video/D3D12VideoEncodeBackend.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

#include <d3d12video.h>
#include <Windows.h>
#include <D3D12Helper/D3D12Core/D3D12Barrier.hpp>
#include <D3D12Helper/D3D12Framework/D3D12Helpers.hpp>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>

namespace D3DVideoEncoderLib {
namespace {

using Microsoft::WRL::ComPtr;

std::string hr_hex(HRESULT hr) {
    std::ostringstream os;
    os << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return os.str();
}

void throw_hr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        std::ostringstream oss;
        oss << what << " failed. HRESULT=" << hr_hex(hr);
        throw D3DVideoEncoderError(oss.str());
    }
}

D3D12_VIDEO_ENCODER_CODEC d3d_codec(VideoCodec codec) {
    switch (codec) {
    case VideoCodec::H264: return D3D12_VIDEO_ENCODER_CODEC_H264;
    case VideoCodec::HEVC: return D3D12_VIDEO_ENCODER_CODEC_HEVC;
    default: throw D3DVideoEncoderError("D3D12VideoEncodeBackend supports only H.264 and HEVC in this phase.");
    }
}

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

uint64_t align_up_u64(uint64_t value, uint64_t alignment) noexcept {
    if (alignment <= 1) return value;
    return ((value + alignment - 1) / alignment) * alignment;
}

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) noexcept {
    D3D12_HEAP_PROPERTIES props = {};
    props.Type = type;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    props.CreationNodeMask = 1;
    props.VisibleNodeMask = 1;
    return props;
}

D3D12_RESOURCE_DESC buffer_desc(uint64_t size) noexcept {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Alignment = 0;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

D3D12_RESOURCE_DESC encode_texture_desc(uint32_t width, uint32_t height, DXGI_FORMAT format) noexcept {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    return desc;
}

D3D12_RESOURCE_BARRIER transition_barrier(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) noexcept {

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

struct CodecProfileStorage {
    D3D12_VIDEO_ENCODER_PROFILE_H264 h264 = D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH;
    D3D12_VIDEO_ENCODER_PROFILE_HEVC hevc = D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN;
    D3D12_VIDEO_ENCODER_PROFILE_DESC desc = {};

    CodecProfileStorage(VideoCodec codec, VideoPixelFormat format) {
        if (codec == VideoCodec::HEVC) {
            hevc = (format == VideoPixelFormat::P010)
                ? D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN10
                : D3D12_VIDEO_ENCODER_PROFILE_HEVC_MAIN;
            desc.DataSize = sizeof(hevc);
            desc.pHEVCProfile = &hevc;
        } else {
            h264 = D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH;
            desc.DataSize = sizeof(h264);
            desc.pH264Profile = &h264;
        }
    }
};

struct CodecLevelStorage {
    D3D12_VIDEO_ENCODER_LEVELS_H264 h264 = D3D12_VIDEO_ENCODER_LEVELS_H264_52;
    D3D12_VIDEO_ENCODER_LEVEL_TIER_CONSTRAINTS_HEVC hevc = {
        D3D12_VIDEO_ENCODER_LEVELS_HEVC_52,
        D3D12_VIDEO_ENCODER_TIER_HEVC_MAIN
    };
    D3D12_VIDEO_ENCODER_LEVEL_SETTING desc = {};

    explicit CodecLevelStorage(VideoCodec codec) {
        if (codec == VideoCodec::HEVC) {
            desc.DataSize = sizeof(hevc);
            desc.pHEVCLevelSetting = &hevc;
        } else {
            desc.DataSize = sizeof(h264);
            desc.pH264LevelSetting = &h264;
        }
    }
};

struct CodecConfigStorage {
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 h264 = {};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC hevc = {};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION desc = {};

    explicit CodecConfigStorage(VideoCodec codec) {
        if (codec == VideoCodec::HEVC) {
            hevc.ConfigurationFlags = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_FLAG_NONE;
            hevc.MinLumaCodingUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_8x8;
            hevc.MaxLumaCodingUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_CUSIZE_64x64;
            hevc.MinLumaTransformUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_4x4;
            hevc.MaxLumaTransformUnitSize = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_HEVC_TUSIZE_32x32;
            hevc.max_transform_hierarchy_depth_inter = 0;
            hevc.max_transform_hierarchy_depth_intra = 0;
            desc.DataSize = sizeof(hevc);
            desc.pHEVCConfig = &hevc;
        } else {
            h264.ConfigurationFlags = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_NONE;
            h264.DirectModeConfig = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES_DISABLED;
            h264.DisableDeblockingFilterConfig = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_SLICES_DEBLOCKING_MODE_0_ALL_LUMA_CHROMA_SLICE_BLOCK_EDGES_ALWAYS_FILTERED;
            desc.DataSize = sizeof(h264);
            desc.pH264Config = &h264;
        }
    }
};

struct CodecGopStorage {
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264 h264 = {};
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_HEVC hevc = {};
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE desc = {};

    CodecGopStorage(VideoCodec codec, uint32_t gopLength) {
        const UINT gop = std::max<uint32_t>(gopLength, 1);
        if (codec == VideoCodec::HEVC) {
            hevc.GOPLength = gop;
            hevc.PPicturePeriod = 1;
            hevc.log2_max_pic_order_cnt_lsb_minus4 = 4;
            desc.DataSize = sizeof(hevc);
            desc.pHEVCGroupOfPictures = &hevc;
        } else {
            h264.GOPLength = gop;
            h264.PPicturePeriod = 1;
            h264.pic_order_cnt_type = 0;
            h264.log2_max_frame_num_minus4 = 4;
            h264.log2_max_pic_order_cnt_lsb_minus4 = 4;
            desc.DataSize = sizeof(h264);
            desc.pH264GroupOfPictures = &h264;
        }
    }
};

struct RateControlStorage {
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CBR cbr = {};
    D3D12_VIDEO_ENCODER_RATE_CONTROL_CQP cqp = {};
    D3D12_VIDEO_ENCODER_RATE_CONTROL desc = {};

    RateControlStorage(const D3D12VideoEncoderDesc& encoderDesc, bool useCbr) {
        desc.Flags = D3D12_VIDEO_ENCODER_RATE_CONTROL_FLAG_NONE;
        desc.TargetFrameRate.Numerator = encoderDesc.frameRateNum;
        desc.TargetFrameRate.Denominator = encoderDesc.frameRateDen;
        if (useCbr) {
            cbr.InitialQP = 26;
            cbr.MinQP = 0;
            cbr.MaxQP = (encoderDesc.internalFormat == VideoPixelFormat::P010) ? 63 : 51;
            cbr.MaxFrameBitSize = 0;
            cbr.TargetBitRate = encoderDesc.bitrate;
            cbr.VBVCapacity = std::max<uint64_t>(encoderDesc.bitrate, 1u);
            cbr.InitialVBVFullness = cbr.VBVCapacity / 2;
            desc.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CBR;
            desc.ConfigParams.DataSize = sizeof(cbr);
            desc.ConfigParams.pConfiguration_CBR = &cbr;
        } else {
            cqp.ConstantQP_FullIntracodedFrame = 22;
            cqp.ConstantQP_InterPredictedFrame_PrevRefOnly = 24;
            cqp.ConstantQP_InterPredictedFrame_BiDirectionalRef = 26;
            desc.Mode = D3D12_VIDEO_ENCODER_RATE_CONTROL_MODE_CQP;
            desc.ConfigParams.DataSize = sizeof(cqp);
            desc.ConfigParams.pConfiguration_CQP = &cqp;
        }
    }
};

bool is_idr_frame(uint64_t frameIndex, uint32_t gopLength) noexcept {
    const uint32_t gop = std::max<uint32_t>(gopLength, 1);
    return (frameIndex % gop) == 0;
}

} // namespace

D3D12VideoEncodeBackend::D3D12VideoEncodeBackend(DebugLog log) : log_(log) {}
D3D12VideoEncodeBackend::~D3D12VideoEncodeBackend() {
    try { close(); } catch (...) {}
}

bool D3D12VideoEncodeBackend::inputAlreadyMatchesInternalFormat() const noexcept {
    return desc_.input.inputFormat == ToDxgiFormat(desc_.internalFormat);
}

uint32_t D3D12VideoEncodeBackend::sourceWidth() const noexcept {
    return desc_.input.sourceWidth != 0 ? desc_.input.sourceWidth : desc_.width;
}

uint32_t D3D12VideoEncodeBackend::sourceHeight() const noexcept {
    return desc_.input.sourceHeight != 0 ? desc_.input.sourceHeight : desc_.height;
}

D3D12CoreLib::Processing::ProcessingRect D3D12VideoEncodeBackend::resolvedSourceRect() const {
    const UINT srcW = sourceWidth();
    const UINT srcH = sourceHeight();
    if (srcW == 0 || srcH == 0) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend sourceWidth/sourceHeight must resolve to non-zero values.");
    }

    if (desc_.input.sourceRect.isEmpty()) {
        return { 0, 0, srcW, srcH };
    }

    const auto& r = desc_.input.sourceRect;
    if (r.x < 0 || r.y < 0 || r.width == 0 || r.height == 0 ||
        static_cast<uint64_t>(r.x) + r.width > srcW ||
        static_cast<uint64_t>(r.y) + r.height > srcH) {
        std::ostringstream oss;
        oss << "D3D12VideoEncodeBackend sourceRect is outside the source texture. source="
            << srcW << "x" << srcH
            << " rect=(" << r.x << "," << r.y << "," << r.width << "," << r.height << ")";
        throw D3DVideoEncoderError(oss.str());
    }
    return { r.x, r.y, r.width, r.height };
}

bool D3D12VideoEncodeBackend::needsResizeOrCrop() const {
    const auto rect = resolvedSourceRect();
    return !RectEqualsFull(rect, sourceWidth(), sourceHeight()) ||
           rect.width != desc_.width ||
           rect.height != desc_.height;
}

bool D3D12VideoEncodeBackend::inputIsRgbaLike() const noexcept {
    return D3D12CoreLib::Processing::IsRgbaLikeFormat(desc_.input.inputFormat);
}

bool D3D12VideoEncodeBackend::inputIsDirectYuvWithProcessing() const noexcept {
    return inputAlreadyMatchesInternalFormat() && D3D12CoreLib::Processing::IsYuv420Format(desc_.input.inputFormat);
}

D3D12CoreLib::Processing::ProcessingFilter D3D12VideoEncodeBackend::processingFilter() const noexcept {
    switch (desc_.input.resizeFilter) {
    case VideoProcessingFilter::Point: return D3D12CoreLib::Processing::ProcessingFilter::Point;
    case VideoProcessingFilter::Linear: return D3D12CoreLib::Processing::ProcessingFilter::Linear;
    default: return D3D12CoreLib::Processing::ProcessingFilter::Linear;
    }
}

void D3D12VideoEncodeBackend::initialize(const D3D12VideoEncoderDesc& desc) {
    desc_ = desc;
    validateDesc();
    queryVideoDevice();
    queryEncodeSupport();
    queryResourceRequirements();
    if (useProcessing_) {
        initializeProcessingIfNeeded();
    }
    createEncoderObjects();
    createQueuesAndCommands();
    createBuffers();
    createReconstructedPictures();
    createFences();
    writer_.open(desc_.outputPath, desc_.width, desc_.height, desc_.frameRateNum, desc_.frameRateDen, desc_.codec);

    open_ = true;
    log_.info(useProcessing_
        ? (inputIsDirectYuvWithProcessing()
            ? "D3D12VideoEncodeBackend initialized with D3D12Processing direct YUV crop/resize -> encode format path"
            : "D3D12VideoEncodeBackend initialized with D3D12Processing RGB/crop/resize -> encode format path")
        : "D3D12VideoEncodeBackend initialized with direct native D3D12 Video Encode input path");
}

void D3D12VideoEncodeBackend::validateDesc() {
    if (!desc_.input.core) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend requires desc.input.core.");
    }
    if (desc_.codec != VideoCodec::H264 && desc_.codec != VideoCodec::HEVC) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend currently supports only H.264 and HEVC.");
    }
    if (desc_.codec == VideoCodec::H264 && desc_.internalFormat != VideoPixelFormat::NV12) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend H.264 requires internalFormat=NV12.");
    }
    if (desc_.codec == VideoCodec::HEVC && desc_.internalFormat != VideoPixelFormat::NV12 && desc_.internalFormat != VideoPixelFormat::P010) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend HEVC requires internalFormat=NV12 or P010.");
    }
    if (desc_.bFrameCount != 0) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend currently requires bFrameCount=0.");
    }
    if ((desc_.width % 2) != 0 || (desc_.height % 2) != 0 || desc_.width == 0 || desc_.height == 0) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend requires non-zero even width/height.");
    }
    if (sourceWidth() == 0 || sourceHeight() == 0) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend sourceWidth/sourceHeight resolved to zero.");
    }

    const bool directMatch = inputAlreadyMatchesInternalFormat();
    const bool rgbaLike = inputIsRgbaLike();
    const bool directYuv = inputIsDirectYuvWithProcessing();
    const bool resizeOrCrop = needsResizeOrCrop();
    useProcessing_ = !directMatch || resizeOrCrop;

    if (directYuv && resizeOrCrop) {
        const auto r = resolvedSourceRect();
        if ((r.x % 2) != 0 || (r.y % 2) != 0 || (r.width % 2) != 0 || (r.height % 2) != 0) {
            throw D3DVideoEncoderError(
                "D3D12VideoEncodeBackend direct NV12/P010 crop/resize requires even sourceRect x/y/width/height.");
        }
    }

    if (!directMatch && !rgbaLike) {
        std::ostringstream oss;
        oss << "D3D12VideoEncodeBackend input.inputFormat is unsupported. "
            << "Direct path requires inputFormat to match internalFormat; conversion path requires RGBA-like input. inputFormat="
            << FormatDxgi(desc_.input.inputFormat);
        throw D3DVideoEncoderError(oss.str());
    }
    if (useProcessing_ && !desc_.input.allowFormatConversion) {
        throw D3DVideoEncoderError(
            "D3D12VideoEncodeBackend requires D3D12Processing because the input format/size/crop does not directly match encode input, "
            "but desc.input.allowFormatConversion=false.");
    }
    if (useProcessing_ && !rgbaLike && !(directYuv && resizeOrCrop)) {
        throw D3DVideoEncoderError(
            "D3D12VideoEncodeBackend D3D12Processing path requires either RGB-like input or direct NV12/P010 input with crop/resize.");
    }
}

void D3D12VideoEncodeBackend::queryVideoDevice() {
    const HRESULT hr = desc_.input.core->GetDevice()->QueryInterface(IID_PPV_ARGS(&videoDevice_));
    if (FAILED(hr) || !videoDevice_) {
        throw D3DVideoEncoderError("ID3D12VideoDevice is not available on this D3D12 device. HRESULT=" + hr_hex(hr));
    }

    const HRESULT hr3 = desc_.input.core->GetDevice()->QueryInterface(IID_PPV_ARGS(&videoDevice3_));
    if (FAILED(hr3) || !videoDevice3_) {
        throw D3DVideoEncoderError("ID3D12VideoDevice3 is not available on this D3D12 device. HRESULT=" + hr_hex(hr3));
    }
    log_.info("D3D12VideoEncodeBackend found ID3D12VideoDevice and ID3D12VideoDevice3");
}

void D3D12VideoEncodeBackend::queryEncodeSupport() {
    capability_ = QueryD3D12VideoEncodeDeviceSupport(
        desc_.input.core,
        desc_.codec,
        desc_.internalFormat,
        desc_.width,
        desc_.height);

    if (!capability_.supported) {
        std::ostringstream oss;
        oss << "D3D12VideoEncodeBackend capability check failed for codec=" << ToString(desc_.codec)
            << ", format=" << ToString(desc_.internalFormat)
            << ", size=" << desc_.width << "x" << desc_.height
            << ": " << capability_.message;
        throw D3DVideoEncoderError(oss.str());
    }

    CodecProfileStorage profile(desc_.codec, desc_.internalFormat);
    CodecConfigStorage codecConfig(desc_.codec);
    CodecGopStorage gop(desc_.codec, desc_.gopLength);
    RateControlStorage rateControl(desc_, capability_.cbrSupported);
    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC resolution = { desc_.width, desc_.height };
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS resolutionLimits = {};

    D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT support = {};
    support.NodeIndex = 0;
    support.Codec = d3d_codec(desc_.codec);
    support.InputFormat = ToDxgiFormat(desc_.internalFormat);
    support.CodecConfiguration = codecConfig.desc;
    support.CodecGopSequence = gop.desc;
    support.RateControl = rateControl.desc;
    support.IntraRefresh = D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE;
    support.SubregionFrameEncoding = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
    support.ResolutionsListCount = 1;
    support.pResolutionList = &resolution;
    support.MaxReferenceFramesInDPB = 1;
    support.pResolutionDependentSupport = &resolutionLimits;

    const HRESULT hr = videoDevice_->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_SUPPORT,
        &support,
        sizeof(support));
    if (FAILED(hr) || (support.SupportFlags & D3D12_VIDEO_ENCODER_SUPPORT_FLAG_GENERAL_SUPPORT_OK) == 0) {
        std::ostringstream oss;
        oss << "D3D12 Video Encoder full support query failed. HRESULT=" << hr_hex(hr)
            << ", ValidationFlags=0x" << std::hex << static_cast<uint64_t>(support.ValidationFlags)
            << ", SupportFlags=0x" << static_cast<uint64_t>(support.SupportFlags);
        throw D3DVideoEncoderError(oss.str());
    }
}

void D3D12VideoEncodeBackend::queryResourceRequirements() {
    CodecProfileStorage profile(desc_.codec, desc_.internalFormat);

    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS req = {};
    req.NodeIndex = 0;
    req.Codec = d3d_codec(desc_.codec);
    req.Profile = profile.desc;
    req.InputFormat = ToDxgiFormat(desc_.internalFormat);
    req.PictureTargetResolution = { desc_.width, desc_.height };

    const HRESULT hr = videoDevice_->CheckFeatureSupport(
        D3D12_FEATURE_VIDEO_ENCODER_RESOURCE_REQUIREMENTS,
        &req,
        sizeof(req));
    if (FAILED(hr) || !req.IsSupported) {
        throw D3DVideoEncoderError("D3D12 Video Encode resource requirements query failed. HRESULT=" + hr_hex(hr));
    }

    bitstreamAlignment_ = std::max<uint32_t>(req.CompressedBitstreamBufferAccessAlignment, 1);
    metadataAlignment_ = std::max<uint32_t>(req.EncoderMetadataBufferAccessAlignment, 1);
    encoderMetadataBufferSize_ = align_up_u64(std::max<uint32_t>(req.MaxEncoderOutputMetadataBufferSize, 4096u), metadataAlignment_);
    resolvedMetadataBufferSize_ = align_up_u64(
        sizeof(D3D12_VIDEO_ENCODER_OUTPUT_METADATA) + sizeof(D3D12_VIDEO_ENCODER_FRAME_SUBREGION_METADATA) * 32ull,
        metadataAlignment_);

    const uint64_t framePixels = static_cast<uint64_t>(desc_.width) * static_cast<uint64_t>(desc_.height);
    const uint64_t bytesPerPixelEstimate = desc_.internalFormat == VideoPixelFormat::P010 ? 6ull : 4ull;
    const uint64_t roughFrameBytes = framePixels * bytesPerPixelEstimate + 1024ull * 1024ull;
    const uint64_t roughRateBytes = (static_cast<uint64_t>(desc_.bitrate) / std::max<uint32_t>(desc_.frameRateNum, 1u)) + 1024ull * 1024ull;
    bitstreamBufferSize_ = align_up_u64(std::max<uint64_t>(roughFrameBytes, roughRateBytes), bitstreamAlignment_);
}

void D3D12VideoEncodeBackend::createEncoderObjects() {
    CodecProfileStorage profile(desc_.codec, desc_.internalFormat);
    CodecLevelStorage level(desc_.codec);
    CodecConfigStorage codecConfig(desc_.codec);
    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC resolution = { desc_.width, desc_.height };

    D3D12_VIDEO_ENCODER_DESC encoderDesc = {};
    encoderDesc.NodeMask = 0;
    encoderDesc.Flags = D3D12_VIDEO_ENCODER_FLAG_NONE;
    encoderDesc.EncodeCodec = d3d_codec(desc_.codec);
    encoderDesc.EncodeProfile = profile.desc;
    encoderDesc.InputFormat = ToDxgiFormat(desc_.internalFormat);
    encoderDesc.CodecConfiguration = codecConfig.desc;
    encoderDesc.MaxMotionEstimationPrecision = D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE_FULL_PIXEL;

    throw_hr(videoDevice3_->CreateVideoEncoder(&encoderDesc, IID_PPV_ARGS(&encoder_)), "CreateVideoEncoder");

    D3D12_VIDEO_ENCODER_HEAP_DESC heapDesc = {};
    heapDesc.NodeMask = 0;
    heapDesc.Flags = D3D12_VIDEO_ENCODER_HEAP_FLAG_NONE;
    heapDesc.EncodeCodec = d3d_codec(desc_.codec);
    heapDesc.EncodeProfile = profile.desc;
    heapDesc.EncodeLevel = level.desc;
    heapDesc.ResolutionsListCount = 1;
    heapDesc.pResolutionList = &resolution;

    throw_hr(videoDevice3_->CreateVideoEncoderHeap(&heapDesc, IID_PPV_ARGS(&encoderHeap_)), "CreateVideoEncoderHeap");
}

void D3D12VideoEncodeBackend::createQueuesAndCommands() {
    ID3D12Device* device = desc_.input.core->GetDevice();

    D3D12_COMMAND_QUEUE_DESC videoQueueDesc = {};
    videoQueueDesc.Type = D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE;
    videoQueueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    videoQueueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    videoQueueDesc.NodeMask = 0;
    throw_hr(device->CreateCommandQueue(&videoQueueDesc, IID_PPV_ARGS(&videoQueue_)), "CreateCommandQueue(VIDEO_ENCODE)");

    throw_hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, IID_PPV_ARGS(&videoAllocator_)), "CreateCommandAllocator(VIDEO_ENCODE)");
    throw_hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, videoAllocator_.Get(), nullptr, IID_PPV_ARGS(&videoCommandList_)), "CreateCommandList(VIDEO_ENCODE)");
    throw_hr(videoCommandList_->Close(), "Close initial video encode command list");

    throw_hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&copyAllocator_)), "CreateCommandAllocator(DIRECT copy)");
    throw_hr(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, copyAllocator_.Get(), nullptr, IID_PPV_ARGS(&copyCommandList_)), "CreateCommandList(DIRECT copy)");
    throw_hr(copyCommandList_->Close(), "Close initial copy command list");
}

void D3D12VideoEncodeBackend::createBuffers() {
    ID3D12Device* device = desc_.input.core->GetDevice();

    const auto defaultHeap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    const auto readbackHeap = heap_properties(D3D12_HEAP_TYPE_READBACK);

    auto bitstreamDesc = buffer_desc(bitstreamBufferSize_);
    throw_hr(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &bitstreamDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&bitstreamBuffer_)), "Create bitstream buffer");
    throw_hr(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bitstreamDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&bitstreamReadback_)), "Create bitstream readback buffer");

    auto encoderMetadataDesc = buffer_desc(encoderMetadataBufferSize_);
    throw_hr(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &encoderMetadataDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&encoderMetadataBuffer_)), "Create encoder metadata buffer");

    auto resolvedMetadataDesc = buffer_desc(resolvedMetadataBufferSize_);
    throw_hr(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &resolvedMetadataDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resolvedMetadataBuffer_)), "Create resolved metadata buffer");
    throw_hr(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &resolvedMetadataDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&resolvedMetadataReadback_)), "Create resolved metadata readback buffer");
}

void D3D12VideoEncodeBackend::createReconstructedPictures() {
    ID3D12Device* device = desc_.input.core->GetDevice();
    const auto defaultHeap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    const auto textureDesc = encode_texture_desc(desc_.width, desc_.height, ToDxgiFormat(desc_.internalFormat));
    for (auto& recon : reconstructedPictures_) {
        throw_hr(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&recon)), "Create reconstructed encode-format texture");
    }
}

void D3D12VideoEncodeBackend::createFences() {
    ID3D12Device* device = desc_.input.core->GetDevice();
    throw_hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&videoFence_)), "Create video fence");
    throw_hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence_)), "Create copy fence");
}

void D3D12VideoEncodeBackend::initializeProcessingIfNeeded() {
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
    processingCommandContext_ = core.CreateDirectContext();

    if (inputIsDirectYuvWithProcessing() && needsResizeOrCrop()) {
        yuvToRgbaTexture_ = formatConverter_.CreateOutputTexture(
            core,
            desc_.width,
            desc_.height,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            D3D12_RESOURCE_STATE_COMMON);
        yuvToRgbaTextureState_ = D3D12_RESOURCE_STATE_COMMON;
    } else if (needsResizeOrCrop()) {
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

void D3D12VideoEncodeBackend::validateInputResource(ID3D12Resource* resource) const {
    if (!resource) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend encode received a null ID3D12Resource.");
    }

    const D3D12_RESOURCE_DESC rd = resource->GetDesc();
    if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        rd.Width != sourceWidth() ||
        rd.Height != sourceHeight() ||
        rd.Format != desc_.input.inputFormat) {
        std::ostringstream oss;
        oss << "D3D12VideoEncodeBackend input resource mismatch. expected="
            << sourceWidth() << "x" << sourceHeight()
            << " inputFormat=" << FormatDxgi(desc_.input.inputFormat)
            << " actual=" << static_cast<uint64_t>(rd.Width) << "x" << rd.Height
            << " format=" << FormatDxgi(rd.Format);
        throw D3DVideoEncoderError(oss.str());
    }

    Microsoft::WRL::ComPtr<ID3D12Device> resourceDevice;
    resource->GetDevice(IID_PPV_ARGS(&resourceDevice));
    if (resourceDevice.Get() != desc_.input.core->GetDevice()) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend input resource belongs to a different ID3D12Device than desc.input.core.");
    }
}

ID3D12Resource* D3D12VideoEncodeBackend::convertToInternalFormat(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState) {
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
    DXGI_FORMAT convertSourceFormat = desc_.input.inputFormat;
    D3D12_RESOURCE_STATES convertSourceState = currentState;

    processingCommandContext_.Reset();

    if (inputIsDirectYuvWithProcessing() && needsResizeOrCrop()) {
        D3D12CoreLib::Processing::D3D12ProcessingStateDesc yuvToRgbaStates = {};
        yuvToRgbaStates.useExplicitStates = true;
        yuvToRgbaStates.srcBefore = currentState;
        yuvToRgbaStates.srcAfter = currentState;
        yuvToRgbaStates.dstBefore = yuvToRgbaTextureState_;
        yuvToRgbaStates.dstAfter = D3D12_RESOURCE_STATE_COMMON;

        D3D12CoreLib::Processing::FusedConvertResizeDesc yuvToRgba = {};
        yuvToRgba.srcFormat = desc_.input.inputFormat;
        yuvToRgba.dstFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        yuvToRgba.color.srcMatrix = ToProcessingMatrix(desc_.colorMatrix);
        yuvToRgba.color.dstMatrix = ToProcessingMatrix(desc_.colorMatrix);
        yuvToRgba.color.srcRange = ToProcessingRange(desc_.colorRange);
        yuvToRgba.color.dstRange = ToProcessingRange(desc_.colorRange);
        yuvToRgba.color.alphaMode = D3D12CoreLib::Processing::ProcessingAlphaMode::Ignore;
        yuvToRgba.filter = processingFilter();
        yuvToRgba.srcRect = srcRect;
        yuvToRgba.dstRect = { 0, 0, desc_.width, desc_.height };
        fusedProcessor_.RecordConvertResize(processingCommandContext_, src, yuvToRgbaTexture_, yuvToRgba, yuvToRgbaStates);

        yuvToRgbaTextureState_ = D3D12_RESOURCE_STATE_COMMON;
        convertSource = &yuvToRgbaTexture_;
        convertSourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        convertSourceState = D3D12_RESOURCE_STATE_COMMON;
    } else if (needsResizeOrCrop()) {
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
            fusedProcessor_.RecordConvertResize(processingCommandContext_, src, resizedTexture_, fused, resizeStates);
        } else {
            D3D12CoreLib::Processing::ResizeDesc resize = {};
            resize.filter = processingFilter();
            resize.srcRect = srcRect;
            resize.dstRect = { 0, 0, desc_.width, desc_.height };
            resizer_.RecordResize(processingCommandContext_, src, resizedTexture_, resize, resizeStates);
        }

        resizedTextureState_ = D3D12_RESOURCE_STATE_COMMON;
        convertSource = &resizedTexture_;
        convertSourceFormat = desc_.input.inputFormat;
        convertSourceState = D3D12_RESOURCE_STATE_COMMON;
    }

    D3D12CoreLib::Processing::FormatConvertDesc convert = {};
    convert.srcFormat = convertSourceFormat;
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

    formatConverter_.RecordConvert(processingCommandContext_, *convertSource, convertedTexture_, convert, convertStates);
    processingCommandContext_.Close();

    ID3D12CommandList* lists[] = { processingCommandContext_.GetCommandList() };
    core.DirectQueue().ExecuteCommandLists(1, lists);
    processingFenceValue_ = core.DirectQueue().Signal();
    core.DirectQueue().WaitForFenceValue(processingFenceValue_);
    processingFenceValue_ = 0;
    convertedTextureState_ = D3D12_RESOURCE_STATE_COMMON;

    return convertedTexture_.Get();
}

void D3D12VideoEncodeBackend::transitionVideo(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (!resource || before == after) return;
    const auto barrier = transition_barrier(resource, before, after);
    videoCommandList_->ResourceBarrier(1, &barrier);
}

void D3D12VideoEncodeBackend::transitionCopy(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (!resource || before == after) return;
    const auto barrier = transition_barrier(resource, before, after);
    copyCommandList_->ResourceBarrier(1, &barrier);
}

void D3D12VideoEncodeBackend::recordEncodeFrame(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, bool restoreInputState) {
    const bool idr = is_idr_frame(frameIndex_, desc_.gopLength) || !hasReferenceFrame_;
    currentReconIndex_ = static_cast<uint32_t>(frameIndex_ % reconstructedPictures_.size());
    previousReconIndex_ = static_cast<uint32_t>((frameIndex_ + reconstructedPictures_.size() - 1) % reconstructedPictures_.size());

    transitionVideo(resource, currentState, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ);
    transitionVideo(bitstreamBuffer_.Get(), bitstreamState_, D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE);
    transitionVideo(encoderMetadataBuffer_.Get(), encoderMetadataState_, D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE);
    transitionVideo(resolvedMetadataBuffer_.Get(), resolvedMetadataState_, D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE);
    transitionVideo(reconstructedPictures_[currentReconIndex_].Get(), reconstructedStates_[currentReconIndex_], D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE);

    if (!idr && hasReferenceFrame_) {
        transitionVideo(reconstructedPictures_[previousReconIndex_].Get(), reconstructedStates_[previousReconIndex_], D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ);
        reconstructedStates_[previousReconIndex_] = D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ;
    }

    CodecGopStorage gop(desc_.codec, desc_.gopLength);
    RateControlStorage rateControl(desc_, capability_.cbrSupported);

    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA picCodecData = {};
    D3D12_VIDEO_ENCODE_REFERENCE_FRAMES referenceFrames = {};
    ID3D12Resource* refResource = nullptr;
    UINT refSubresource = 0;

    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264 h264Pic = {};
    UINT h264List0Index = 0;
    D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_H264 h264RefDesc = {};

    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC hevcPic = {};
    UINT hevcList0Index = 0;
    D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_HEVC hevcRefDesc = {};

    if (desc_.codec == VideoCodec::HEVC) {
        hevcPic.Flags = D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_HEVC_FLAG_NONE;
        hevcPic.FrameType = idr ? D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_IDR_FRAME : D3D12_VIDEO_ENCODER_FRAME_TYPE_HEVC_P_FRAME;
        hevcPic.slice_pic_parameter_set_id = 0;
        hevcPic.PictureOrderCountNumber = static_cast<UINT>(frameIndex_);
        hevcPic.TemporalLayerIndex = 0;

        if (!idr && hasReferenceFrame_) {
            hevcRefDesc.ReconstructedPictureResourceIndex = 0;
            hevcRefDesc.IsRefUsedByCurrentPic = TRUE;
            hevcRefDesc.IsLongTermReference = FALSE;
            hevcRefDesc.PictureOrderCountNumber = static_cast<UINT>(frameIndex_ - 1);
            hevcRefDesc.TemporalLayerIndex = 0;

            hevcPic.List0ReferenceFramesCount = 1;
            hevcPic.pList0ReferenceFrames = &hevcList0Index;
            hevcPic.ReferenceFramesReconPictureDescriptorsCount = 1;
            hevcPic.pReferenceFramesReconPictureDescriptors = &hevcRefDesc;
            refResource = reconstructedPictures_[previousReconIndex_].Get();
        }

        picCodecData.DataSize = sizeof(hevcPic);
        picCodecData.pHEVCPicData = &hevcPic;
    } else {
        h264Pic.Flags = D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264_FLAG_NONE;
        h264Pic.FrameType = idr ? D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME : D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME;
        h264Pic.pic_parameter_set_id = 0;
        h264Pic.idr_pic_id = static_cast<UINT>(frameIndex_ & 0xFFFFu);
        h264Pic.PictureOrderCountNumber = static_cast<UINT>(frameIndex_);
        h264Pic.FrameDecodingOrderNumber = static_cast<UINT>(frameIndex_);
        h264Pic.TemporalLayerIndex = 0;

        if (!idr && hasReferenceFrame_) {
            h264RefDesc.ReconstructedPictureResourceIndex = 0;
            h264RefDesc.IsLongTermReference = FALSE;
            h264RefDesc.LongTermPictureIdx = 0;
            h264RefDesc.PictureOrderCountNumber = static_cast<UINT>(frameIndex_ - 1);
            h264RefDesc.FrameDecodingOrderNumber = static_cast<UINT>(frameIndex_ - 1);
            h264RefDesc.TemporalLayerIndex = 0;

            h264Pic.List0ReferenceFramesCount = 1;
            h264Pic.pList0ReferenceFrames = &h264List0Index;
            h264Pic.ReferenceFramesReconPictureDescriptorsCount = 1;
            h264Pic.pReferenceFramesReconPictureDescriptors = &h264RefDesc;
            refResource = reconstructedPictures_[previousReconIndex_].Get();
        }

        picCodecData.DataSize = sizeof(h264Pic);
        picCodecData.pH264PicData = &h264Pic;
    }

    if (refResource) {
        referenceFrames.NumTexture2Ds = 1;
        referenceFrames.ppTexture2Ds = &refResource;
        referenceFrames.pSubresources = &refSubresource;
    }

    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_DESC pictureControl = {};
    pictureControl.IntraRefreshFrameIndex = 0;
    pictureControl.Flags = D3D12_VIDEO_ENCODER_PICTURE_CONTROL_FLAG_USED_AS_REFERENCE_PICTURE;
    pictureControl.PictureControlCodecData = picCodecData;
    pictureControl.ReferenceFrames = referenceFrames;

    D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_DESC sequence = {};
    sequence.Flags = (frameIndex_ == 0)
        ? static_cast<D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAGS>(
              D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_RESOLUTION_CHANGE |
              D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_RATE_CONTROL_CHANGE |
              D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_SUBREGION_LAYOUT_CHANGE |
              D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_GOP_SEQUENCE_CHANGE)
        : D3D12_VIDEO_ENCODER_SEQUENCE_CONTROL_FLAG_NONE;
    sequence.IntraRefreshConfig.Mode = D3D12_VIDEO_ENCODER_INTRA_REFRESH_MODE_NONE;
    sequence.IntraRefreshConfig.IntraRefreshDuration = 0;
    sequence.RateControl = rateControl.desc;
    sequence.PictureTargetResolution = { desc_.width, desc_.height };
    sequence.SelectedLayoutMode = D3D12_VIDEO_ENCODER_FRAME_SUBREGION_LAYOUT_MODE_FULL_FRAME;
    sequence.CodecGopSequence = gop.desc;

    D3D12_VIDEO_ENCODER_ENCODEFRAME_INPUT_ARGUMENTS input = {};
    input.SequenceControlDesc = sequence;
    input.PictureControlDesc = pictureControl;
    input.pInputFrame = resource;
    input.InputFrameSubresource = 0;
    input.CurrentFrameBitstreamMetadataSize = 0;

    D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS output = {};
    output.Bitstream.pBuffer = bitstreamBuffer_.Get();
    output.Bitstream.FrameStartOffset = 0;
    output.ReconstructedPicture.pReconstructedPicture = reconstructedPictures_[currentReconIndex_].Get();
    output.ReconstructedPicture.ReconstructedPictureSubresource = 0;
    output.EncoderOutputMetadata.pBuffer = encoderMetadataBuffer_.Get();
    output.EncoderOutputMetadata.Offset = 0;

    videoCommandList_->EncodeFrame(encoder_.Get(), encoderHeap_.Get(), &input, &output);

    transitionVideo(encoderMetadataBuffer_.Get(), D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ);

    CodecProfileStorage profile(desc_.codec, desc_.internalFormat);
    D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS resolveInput = {};
    resolveInput.EncoderCodec = d3d_codec(desc_.codec);
    resolveInput.EncoderProfile = profile.desc;
    resolveInput.EncoderInputFormat = ToDxgiFormat(desc_.internalFormat);
    resolveInput.EncodedPictureEffectiveResolution = { desc_.width, desc_.height };
    resolveInput.HWLayoutMetadata.pBuffer = encoderMetadataBuffer_.Get();
    resolveInput.HWLayoutMetadata.Offset = 0;

    D3D12_VIDEO_ENCODER_RESOLVE_METADATA_OUTPUT_ARGUMENTS resolveOutput = {};
    resolveOutput.ResolvedLayoutMetadata.pBuffer = resolvedMetadataBuffer_.Get();
    resolveOutput.ResolvedLayoutMetadata.Offset = 0;
    videoCommandList_->ResolveEncoderOutputMetadata(&resolveInput, &resolveOutput);

    if (restoreInputState) {
        transitionVideo(resource, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ, currentState);
    }

    bitstreamState_ = D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;
    encoderMetadataState_ = D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ;
    resolvedMetadataState_ = D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;
    reconstructedStates_[currentReconIndex_] = D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE;
}

void D3D12VideoEncodeBackend::signalVideoAndWaitOnCopy() {
    const uint64_t signalValue = ++videoFenceValue_;
    throw_hr(videoQueue_->Signal(videoFence_.Get(), signalValue), "videoQueue Signal");
    throw_hr(desc_.input.core->GetDirectCommandQueue()->Wait(videoFence_.Get(), signalValue), "directQueue Wait(videoFence)");
}

void D3D12VideoEncodeBackend::copyOutputsToReadback() {
    throw_hr(copyAllocator_->Reset(), "copyAllocator Reset");
    throw_hr(copyCommandList_->Reset(copyAllocator_.Get(), nullptr), "copyCommandList Reset");

    transitionCopy(bitstreamBuffer_.Get(), bitstreamState_, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transitionCopy(resolvedMetadataBuffer_.Get(), resolvedMetadataState_, D3D12_RESOURCE_STATE_COPY_SOURCE);
    bitstreamState_ = D3D12_RESOURCE_STATE_COPY_SOURCE;
    resolvedMetadataState_ = D3D12_RESOURCE_STATE_COPY_SOURCE;

    copyCommandList_->CopyBufferRegion(bitstreamReadback_.Get(), 0, bitstreamBuffer_.Get(), 0, bitstreamBufferSize_);
    copyCommandList_->CopyBufferRegion(resolvedMetadataReadback_.Get(), 0, resolvedMetadataBuffer_.Get(), 0, resolvedMetadataBufferSize_);

    throw_hr(copyCommandList_->Close(), "copyCommandList Close");
    ID3D12CommandList* lists[] = { copyCommandList_.Get() };
    desc_.input.core->GetDirectCommandQueue()->ExecuteCommandLists(1, lists);
}

void D3D12VideoEncodeBackend::waitForCopyQueue() {
    const uint64_t signalValue = ++copyFenceValue_;
    ID3D12CommandQueue* queue = desc_.input.core->GetDirectCommandQueue();
    throw_hr(queue->Signal(copyFence_.Get(), signalValue), "directQueue Signal(copyFence)");
    if (copyFence_->GetCompletedValue() < signalValue) {
        void* eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            throw D3DVideoEncoderError("CreateEventW failed while waiting for D3D12 Video Encode copy fence.");
        }
        throw_hr(copyFence_->SetEventOnCompletion(signalValue, eventHandle), "copyFence SetEventOnCompletion");
        WaitForSingleObject(static_cast<HANDLE>(eventHandle), INFINITE);
        CloseHandle(static_cast<HANDLE>(eventHandle));
    }
}

void D3D12VideoEncodeBackend::writeResolvedBitstream(int64_t timestamp100ns, int64_t duration100ns) {
    D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(sizeof(D3D12_VIDEO_ENCODER_OUTPUT_METADATA)) };
    void* metadataMapped = nullptr;
    throw_hr(resolvedMetadataReadback_->Map(0, &readRange, &metadataMapped), "Map resolved metadata readback");
    const auto* metadata = static_cast<const D3D12_VIDEO_ENCODER_OUTPUT_METADATA*>(metadataMapped);
    const D3D12_VIDEO_ENCODER_OUTPUT_METADATA metadataCopy = *metadata;
    D3D12_RANGE emptyRange = { 0, 0 };
    resolvedMetadataReadback_->Unmap(0, &emptyRange);

    if (metadataCopy.EncodeErrorFlags != D3D12_VIDEO_ENCODER_ENCODE_ERROR_FLAG_NO_ERROR) {
        std::ostringstream oss;
        oss << "D3D12 Video Encode reported encode error flags=0x" << std::hex << metadataCopy.EncodeErrorFlags;
        throw D3DVideoEncoderError(oss.str());
    }

    const uint64_t bytesToWrite = metadataCopy.EncodedBitstreamWrittenBytesCount;
    if (bytesToWrite == 0) {
        return;
    }
    if (bytesToWrite > bitstreamBufferSize_) {
        std::ostringstream oss;
        oss << "D3D12 Video Encode reported bitstream size " << bytesToWrite
            << " larger than output buffer " << bitstreamBufferSize_;
        throw D3DVideoEncoderError(oss.str());
    }

    D3D12_RANGE bitstreamRange = { 0, static_cast<SIZE_T>(bytesToWrite) };
    void* bitstreamMapped = nullptr;
    throw_hr(bitstreamReadback_->Map(0, &bitstreamRange, &bitstreamMapped), "Map bitstream readback");
    const auto* bitstream = static_cast<const uint8_t*>(bitstreamMapped);
    try {
        writer_.writeAccessUnit(bitstream, static_cast<size_t>(bytesToWrite), timestamp100ns, duration100ns);
    } catch (...) {
        bitstreamReadback_->Unmap(0, &emptyRange);
        throw;
    }
    bitstreamReadback_->Unmap(0, &emptyRange);
}

void D3D12VideoEncodeBackend::encode(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns, int64_t duration100ns) {
    if (!open_) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend::encode called before initialize.");
    }
    if (!resource) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend::encode received a null resource.");
    }

    ID3D12Resource* encodeResource = resource;
    D3D12_RESOURCE_STATES encodeState = currentState;
    bool restoreEncodeResourceState = desc_.input.restoreStateAfterEncode;
    if (useProcessing_) {
        encodeResource = convertToInternalFormat(resource, currentState);
        encodeState = D3D12_RESOURCE_STATE_COMMON;
        restoreEncodeResourceState = true;
    } else {
        validateInputResource(resource);
    }

    throw_hr(videoAllocator_->Reset(), "videoAllocator Reset");
    throw_hr(videoCommandList_->Reset(videoAllocator_.Get()), "videoCommandList Reset");
    recordEncodeFrame(encodeResource, encodeState, restoreEncodeResourceState);
    throw_hr(videoCommandList_->Close(), "videoCommandList Close");

    ID3D12CommandList* lists[] = { videoCommandList_.Get() };
    videoQueue_->ExecuteCommandLists(1, lists);

    signalVideoAndWaitOnCopy();
    copyOutputsToReadback();
    waitForCopyQueue();
    writeResolvedBitstream(timestamp100ns, duration100ns);

    if (useProcessing_) {
        convertedTextureState_ = D3D12_RESOURCE_STATE_COMMON;
        yuvToRgbaTextureState_ = D3D12_RESOURCE_STATE_COMMON;
    }

    hasReferenceFrame_ = true;
    ++frameIndex_;
}

void D3D12VideoEncodeBackend::flush() {
    if (writer_.isOpen()) {
        writer_.flush();
    }
}

void D3D12VideoEncodeBackend::destroyObjects() noexcept {
    if (processingFenceValue_ != 0 && desc_.input.core) {
        desc_.input.core->DirectQueue().WaitForFenceValue(processingFenceValue_);
        processingFenceValue_ = 0;
    }
    writer_.close();
    convertedTexture_ = {};
    yuvToRgbaTexture_ = {};
    resizedTexture_ = {};
    for (auto& recon : reconstructedPictures_) recon.Reset();
    resolvedMetadataReadback_.Reset();
    resolvedMetadataBuffer_.Reset();
    encoderMetadataBuffer_.Reset();
    bitstreamReadback_.Reset();
    bitstreamBuffer_.Reset();
    copyFence_.Reset();
    videoFence_.Reset();
    copyCommandList_.Reset();
    copyAllocator_.Reset();
    videoCommandList_.Reset();
    videoAllocator_.Reset();
    videoQueue_.Reset();
    encoderHeap_.Reset();
    encoder_.Reset();
    videoDevice3_.Reset();
    videoDevice_.Reset();
}

void D3D12VideoEncodeBackend::close() {
    if (!open_) {
        destroyObjects();
        return;
    }
    std::exception_ptr pending;
    try {
        flush();
    } catch (...) {
        pending = std::current_exception();
    }
    open_ = false;
    destroyObjects();
    if (pending) {
        std::rethrow_exception(pending);
    }
}

} // namespace D3DVideoEncoderLib
