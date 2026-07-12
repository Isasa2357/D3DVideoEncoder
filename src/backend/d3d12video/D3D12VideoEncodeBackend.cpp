#include "backend/d3d12video/D3D12VideoEncodeBackend.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

#include "backend/d3d12video/D3D12VideoEncodeH264ParameterSets.hpp"

#include <d3d12video.h>
#include <Windows.h>
#include <D3D12Helper/D3D12Core/D3D12BarrierBatch.hpp>
#include <D3D12Helper/D3D12Framework/D3D12Helpers.hpp>
#include <D3D12Helper/D3D12Gpu/D3D12ResourceCreate.hpp>
#include <D3D12Helper/D3D12Gpu/D3D12ResourceValidation.hpp>
#include <D3D12Helper/D3D12Gpu/D3D12ResourceView.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <sstream>
#include <string>
#include <utility>

namespace D3DVideoEncoderLib {
namespace {

constexpr UCHAR kH264Log2MaxFrameNumMinus4 = 12;
constexpr UCHAR kH264Log2MaxPicOrderCntLsbMinus4 = 12;

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

std::string FormatResourceState(D3D12_RESOURCE_STATES state) {
    switch (state) {
    case D3D12_RESOURCE_STATE_COMMON: return "COMMON";
    case D3D12_RESOURCE_STATE_COPY_SOURCE: return "COPY_SOURCE";
    case D3D12_RESOURCE_STATE_COPY_DEST: return "COPY_DEST";
    case D3D12_RESOURCE_STATE_RENDER_TARGET: return "RENDER_TARGET";
    case D3D12_RESOURCE_STATE_UNORDERED_ACCESS: return "UNORDERED_ACCESS";
    case D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ: return "VIDEO_ENCODE_READ";
    case D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE: return "VIDEO_ENCODE_WRITE";
    default: {
        std::ostringstream oss;
        oss << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(state);
        return oss.str();
    }
    }
}

std::string FormatResourceStateHex(D3D12_RESOURCE_STATES state) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(state);
    return oss.str();
}

std::string FormatResourcePointer(ID3D12Resource* resource) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << reinterpret_cast<std::uintptr_t>(resource);
    return oss.str();
}

bool ShouldTraceFrame(uint64_t frameIndex) noexcept {
    return frameIndex < 3;
}

std::string FormatFenceProgress(const char* label, const D3D12CoreLib::D3D12QueueSyncPoint& point) {
    std::ostringstream oss;
    oss << label
        << " value=" << point.GetValue()
        << " completed=" << (point.GetFence() ? point.GetFence()->GetCompletedValue() : 0);
    return oss.str();
}

std::string FormatFenceProgress(const char* label, UINT64 value, ID3D12Fence* fence) {
    std::ostringstream oss;
    oss << label
        << " value=" << value
        << " completed=" << (fence ? fence->GetCompletedValue() : 0);
    return oss.str();
}

std::string FormatTransitionDiagnostic(
    const char* commandListType,
    uint64_t frameIndex,
    const char* logicalName,
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {

    std::ostringstream oss;
    oss << "Barrier frame=" << frameIndex
        << " commandList=" << commandListType
        << " resource=" << (logicalName ? logicalName : "(unnamed)")
        << " ptr=" << FormatResourcePointer(resource)
        << " before=" << FormatResourceState(before)
        << "(" << FormatResourceStateHex(before) << ")"
        << " after=" << FormatResourceState(after)
        << "(" << FormatResourceStateHex(after) << ")";
    return oss.str();
}

bool RectEqualsFull(const D3D12CoreLib::Processing::ProcessingRect& r, UINT width, UINT height) noexcept {
    return r.x == 0 && r.y == 0 && r.width == width && r.height == height;
}

uint64_t align_up_u64(uint64_t value, uint64_t alignment) noexcept {
    if (alignment <= 1) return value;
    return ((value + alignment - 1) / alignment) * alignment;
}

uint8_t h264_level_idc(D3D12_VIDEO_ENCODER_LEVELS_H264 level) noexcept {
    switch (level) {
    case D3D12_VIDEO_ENCODER_LEVELS_H264_1: return 10;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_1b: return 9;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_11: return 11;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_12: return 12;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_13: return 13;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_2: return 20;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_21: return 21;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_22: return 22;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_3: return 30;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_31: return 31;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_32: return 32;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_4: return 40;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_41: return 41;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_42: return 42;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_5: return 50;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_51: return 51;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_52: return 52;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_6: return 60;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_61: return 61;
    case D3D12_VIDEO_ENCODER_LEVELS_H264_62: return 62;
    default: return 52;
    }
}

H264ParameterSetConfig make_h264_parameter_set_config(const D3D12VideoEncoderDesc& desc) {
    H264ParameterSetConfig config;
    config.width = desc.width;
    config.height = desc.height;
    config.profileIdc = 100;
    config.constraintFlags = 0;
    config.levelIdc = h264_level_idc(D3D12_VIDEO_ENCODER_LEVELS_H264_52);
    config.seqParameterSetId = 0;
    config.picParameterSetId = 0;
    config.chromaFormatIdc = 1;
    config.bitDepthLumaMinus8 = 0;
    config.bitDepthChromaMinus8 = 0;
    config.log2MaxFrameNumMinus4 = kH264Log2MaxFrameNumMinus4;
    config.picOrderCntType = 0;
    config.log2MaxPicOrderCntLsbMinus4 = kH264Log2MaxPicOrderCntLsbMinus4;
    config.maxNumRefFrames = 2;
    config.entropyCodingModeFlag = false;
    config.bottomFieldPicOrderInFramePresentFlag = false;
    config.deblockingFilterControlPresentFlag = true;
    config.constrainedIntraPredFlag = false;
    config.transform8x8ModeFlag = false;
    return config;
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
            h264.log2_max_frame_num_minus4 = kH264Log2MaxFrameNumMinus4;
            h264.log2_max_pic_order_cnt_lsb_minus4 = kH264Log2MaxPicOrderCntLsbMinus4;
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

uint32_t next_h264_idr_pic_id(uint32_t current) noexcept {
    constexpr uint32_t kMaxIdrPicId = 65535;
    return (current >= kMaxIdrPicId) ? 0 : (current + 1);
}

D3D12CoreLib::D3D12Resource create_buffer(
    D3D12CoreLib::D3D12Core& core,
    uint64_t sizeBytes,
    D3D12_RESOURCE_STATES initialState,
    const char* label) {

    D3D12CoreLib::D3D12BufferCreateDesc desc = {};
    desc.sizeBytes = sizeBytes;
    desc.initialState = initialState;

    try {
        return D3D12CoreLib::CreateBufferDetailed(core, desc);
    } catch (const std::exception& e) {
        throw D3DVideoEncoderError(std::string(label) + " failed: " + e.what());
    }
}

D3D12CoreLib::D3D12Resource create_encode_texture(
    D3D12CoreLib::D3D12Core& core,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    D3D12_RESOURCE_STATES initialState,
    const char* label) {

    D3D12CoreLib::D3D12Texture2DCreateDesc desc = {};
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.initialState = initialState;

    try {
        return D3D12CoreLib::CreateTexture2DDetailed(core, desc);
    } catch (const std::exception& e) {
        throw D3DVideoEncoderError(std::string(label) + " failed: " + e.what());
    }
}

void initialize_readback(
    D3D12CoreLib::D3D12ReadbackBuffer& readback,
    ID3D12Device* device,
    uint64_t sizeBytes,
    const char* label) {

    try {
        readback.Initialize(device, sizeBytes);
    } catch (const std::exception& e) {
        throw D3DVideoEncoderError(std::string(label) + " failed: " + e.what());
    }
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
    frameIndex_ = 0;
    hasReferenceFrame_ = false;
    h264CurrentGopStartFrame_ = 0;
    h264NextIdrPicId_ = 0;
    h264PreviousReferenceDecodingOrder_ = 0;
    h264PreviousReferencePoc_ = 0;
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
    writer_.open(desc_.outputPath, desc_.width, desc_.height, desc_.frameRateNum, desc_.frameRateDen, desc_.codec);
    if (desc_.codec == VideoCodec::H264) {
        writer_.configureH264ParameterSets(make_h264_parameter_set_config(desc_));
    }

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
    CodecProfileStorage suggestedProfile(desc_.codec, desc_.internalFormat);
    CodecLevelStorage suggestedLevel(desc_.codec);
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
    support.SuggestedProfile = suggestedProfile.desc;
    support.SuggestedLevel = suggestedLevel.desc;
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

    try {
        videoQueue_.Initialize(device, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE);
        videoAllocator_.Initialize(device, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE);
        videoCommandList_ = D3D12CoreLib::CreateTypedCommandList<ID3D12VideoEncodeCommandList2>(
            device,
            D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE,
            videoAllocator_);
        throw_hr(videoCommandList_->Close(), "Close initial video encode command list");

        copyAllocator_.Initialize(device, D3D12_COMMAND_LIST_TYPE_DIRECT);
        copyCommandList_ = D3D12CoreLib::CreateTypedCommandList<ID3D12GraphicsCommandList>(
            device,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            copyAllocator_);
        throw_hr(copyCommandList_->Close(), "Close initial copy command list");

        // The largest ordered phases contain six video transitions and two copy
        // transitions. Reserve once so Clear()/Transition() never allocate per frame.
        videoBarriers_.Reserve(6);
        copyBarriers_.Reserve(2);
    } catch (const D3DVideoEncoderError&) {
        throw;
    } catch (const std::exception& e) {
        throw D3DVideoEncoderError(std::string("Create D3D12 Video Encode queues/command lists failed: ") + e.what());
    }
}

void D3D12VideoEncodeBackend::createBuffers() {
    auto& core = *desc_.input.core;
    ID3D12Device* device = core.GetDevice();

    bitstreamBuffer_ = create_buffer(core, bitstreamBufferSize_, D3D12_RESOURCE_STATE_COMMON, "Create bitstream buffer");
    initialize_readback(bitstreamReadback_, device, bitstreamBufferSize_, "Create bitstream readback buffer");

    encoderMetadataBuffer_ = create_buffer(core, encoderMetadataBufferSize_, D3D12_RESOURCE_STATE_COMMON, "Create encoder metadata buffer");
    resolvedMetadataBuffer_ = create_buffer(core, resolvedMetadataBufferSize_, D3D12_RESOURCE_STATE_COMMON, "Create resolved metadata buffer");
    initialize_readback(resolvedMetadataReadback_, device, resolvedMetadataBufferSize_, "Create resolved metadata readback buffer");
}

void D3D12VideoEncodeBackend::createReconstructedPictures() {
    auto& core = *desc_.input.core;
    for (auto& recon : reconstructedPictures_) {
        recon = create_encode_texture(
            core,
            desc_.width,
            desc_.height,
            ToDxgiFormat(desc_.internalFormat),
            D3D12_RESOURCE_STATE_COMMON,
            "Create reconstructed encode-format texture");
    }
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

    D3D12CoreLib::D3D12Texture2DRequirement requirement = {};
    requirement.width = sourceWidth();
    requirement.height = sourceHeight();
    requirement.format = desc_.input.inputFormat;
    requirement.expectedDevice = desc_.input.core->GetDevice();

    const auto validation = D3D12CoreLib::ValidateTexture2DView(
        D3D12CoreLib::D3D12ResourceView(resource),
        requirement);
    if (!validation) {
        std::ostringstream oss;
        oss << "D3D12VideoEncodeBackend input resource validation failed: " << validation.Message();
        throw D3DVideoEncoderError(oss.str());
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

    const auto srcRect = resolvedSourceRect();
    D3D12CoreLib::D3D12ResourceView srcView(resource);
    D3D12CoreLib::D3D12ResourceView convertSource = srcView;
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
        fusedProcessor_.RecordConvertResizeView(
            processingCommandContext_,
            srcView,
            D3D12CoreLib::D3D12ResourceView(yuvToRgbaTexture_),
            yuvToRgba,
            yuvToRgbaStates);

        yuvToRgbaTextureState_ = D3D12_RESOURCE_STATE_COMMON;
        convertSource = D3D12CoreLib::D3D12ResourceView(yuvToRgbaTexture_);
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
            fusedProcessor_.RecordConvertResizeView(
                processingCommandContext_,
                srcView,
                D3D12CoreLib::D3D12ResourceView(resizedTexture_),
                fused,
                resizeStates);
        } else {
            D3D12CoreLib::Processing::ResizeDesc resize = {};
            resize.filter = processingFilter();
            resize.srcRect = srcRect;
            resize.dstRect = { 0, 0, desc_.width, desc_.height };
            resizer_.RecordResizeView(
                processingCommandContext_,
                srcView,
                D3D12CoreLib::D3D12ResourceView(resizedTexture_),
                resize,
                resizeStates);
        }

        resizedTextureState_ = D3D12_RESOURCE_STATE_COMMON;
        convertSource = D3D12CoreLib::D3D12ResourceView(resizedTexture_);
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

    formatConverter_.RecordConvertView(
        processingCommandContext_,
        convertSource,
        D3D12CoreLib::D3D12ResourceView(convertedTexture_),
        convert,
        convertStates);
    processingCommandContext_.Close();

    ID3D12CommandList* lists[] = { processingCommandContext_.GetCommandList() };
    core.DirectQueue().ExecuteCommandLists(1, lists);
    processingFenceValue_ = core.DirectQueue().Signal();
    core.DirectQueue().WaitForFenceValue(processingFenceValue_);
    processingFenceValue_ = 0;
    convertedTextureState_ = D3D12_RESOURCE_STATE_COMMON;

    return convertedTexture_.Get();
}

void D3D12VideoEncodeBackend::transitionVideo(const char* logicalName, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (!resource || before == after) return;
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream oss;
        oss << "Stage frame=" << frameIndex_
            << " commandList=VIDEO_ENCODE"
            << " queue barrier"
            << " resource=" << (logicalName ? logicalName : "(unnamed)")
            << " ptr=" << FormatResourcePointer(resource)
            << " before=" << FormatResourceState(before) << "(" << FormatResourceStateHex(before) << ")"
            << " after=" << FormatResourceState(after) << "(" << FormatResourceStateHex(after) << ")";
        log_.info(oss.str());
    }
    if (videoBarriers_.Transition(resource, before, after) && ShouldTraceFrame(frameIndex_)) {
        log_.info(FormatTransitionDiagnostic("VIDEO_ENCODE", frameIndex_, logicalName, resource, before, after));
    }
}

bool D3D12VideoEncodeBackend::transitionCopy(const char* logicalName, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (!resource || before == after) return false;
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream oss;
        oss << "Stage frame=" << frameIndex_
            << " commandList=DIRECT"
            << " queue barrier"
            << " resource=" << (logicalName ? logicalName : "(unnamed)")
            << " ptr=" << FormatResourcePointer(resource)
            << " before=" << FormatResourceState(before) << "(" << FormatResourceStateHex(before) << ")"
            << " after=" << FormatResourceState(after) << "(" << FormatResourceStateHex(after) << ")";
        log_.info(oss.str());
    }
    const bool added = copyBarriers_.Transition(resource, before, after);
    if (added && ShouldTraceFrame(frameIndex_)) {
        log_.info(FormatTransitionDiagnostic("DIRECT", frameIndex_, logicalName, resource, before, after));
    }
    return added;
}

void D3D12VideoEncodeBackend::recordVideoBarrierPhase() {
    if (videoBarriers_.Empty()) return;
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream oss;
        oss << "Stage frame=" << frameIndex_
            << " commandList=VIDEO_ENCODE record barrier phase count=" << videoBarriers_.Count();
        log_.info(oss.str());
    }
    videoCommandList_->ResourceBarrier(videoBarriers_.Count(), videoBarriers_.Data());
}

void D3D12VideoEncodeBackend::recordCopyBarrierPhase() {
    if (copyBarriers_.Empty()) return;
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream oss;
        oss << "Stage frame=" << frameIndex_
            << " commandList=DIRECT record barrier phase count=" << copyBarriers_.Count();
        log_.info(oss.str());
    }
    copyCommandList_->ResourceBarrier(copyBarriers_.Count(), copyBarriers_.Data());
}

void D3D12VideoEncodeBackend::recordEncodeFrame(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, bool restoreInputState) {
    const bool idr = is_idr_frame(frameIndex_, desc_.gopLength) || !hasReferenceFrame_;
    if (desc_.codec == VideoCodec::H264 && idr) {
        h264CurrentGopStartFrame_ = frameIndex_;
    }
    const uint64_t h264FrameOffset = frameIndex_ - h264CurrentGopStartFrame_;
    const uint32_t h264FrameInGop = static_cast<uint32_t>(std::min<uint64_t>(h264FrameOffset, UINT32_MAX));
    const uint32_t h264CurrentDecodingOrder = idr ? 0u : h264FrameInGop;
    const uint32_t h264CurrentPoc = idr ? 0u : h264FrameInGop;
    const uint32_t h264CurrentIdrPicId = h264NextIdrPicId_;
    const uint32_t h264ReferenceDecodingOrder = h264PreviousReferenceDecodingOrder_;
    const uint32_t h264ReferencePoc = h264PreviousReferencePoc_;

    currentReconIndex_ = static_cast<uint32_t>(frameIndex_ % reconstructedPictures_.size());
    previousReconIndex_ = static_cast<uint32_t>((frameIndex_ + reconstructedPictures_.size() - 1) % reconstructedPictures_.size());
    const UINT referenceCount = (!idr && hasReferenceFrame_) ? 1u : 0u;

    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream oss;
        oss << "FrameDiagnostic frame=" << frameIndex_
            << " type=" << (idr ? "IDR" : "P")
            << " idr=" << (idr ? "true" : "false")
            << " appFrameIndex=" << frameIndex_
            << " frameInGop=" << h264FrameInGop
            << " frame_num=" << h264CurrentDecodingOrder
            << " poc=" << h264CurrentPoc
            << " idr_pic_id=" << h264CurrentIdrPicId
            << " previousReferenceFrameNum=" << h264ReferenceDecodingOrder
            << " previousReferencePoc=" << h264ReferencePoc
            << " currentReconIndex=" << currentReconIndex_
            << " previousReconIndex=" << previousReconIndex_
            << " referenceCount=" << referenceCount
            << " input=" << FormatResourcePointer(resource)
            << " bitstream=" << FormatResourcePointer(bitstreamBuffer_.Get())
            << " encoderMetadata=" << FormatResourcePointer(encoderMetadataBuffer_.Get())
            << " resolvedMetadata=" << FormatResourcePointer(resolvedMetadataBuffer_.Get())
            << " currentRecon=" << FormatResourcePointer(reconstructedPictures_[currentReconIndex_].Get())
            << " previousRecon=" << FormatResourcePointer(reconstructedPictures_[previousReconIndex_].Get())
            << " currentEqualsPrevious="
            << ((reconstructedPictures_[currentReconIndex_].Get() == reconstructedPictures_[previousReconIndex_].Get()) ? "yes" : "no")
            << " inputSubresource=0"
            << " reconSubresource=0"
            << " refSubresource=0";
        log_.info(oss.str());

        std::ostringstream states;
        states << "FrameStateBeforeBarriers frame=" << frameIndex_
               << " input=" << FormatResourceState(currentState) << "(" << FormatResourceStateHex(currentState) << ")"
               << " bitstream=" << FormatResourceState(bitstreamState_) << "(" << FormatResourceStateHex(bitstreamState_) << ")"
               << " encoderMetadata=" << FormatResourceState(encoderMetadataState_) << "(" << FormatResourceStateHex(encoderMetadataState_) << ")"
               << " resolvedMetadata=" << FormatResourceState(resolvedMetadataState_) << "(" << FormatResourceStateHex(resolvedMetadataState_) << ")"
               << " currentRecon=" << FormatResourceState(reconstructedStates_[currentReconIndex_]) << "(" << FormatResourceStateHex(reconstructedStates_[currentReconIndex_]) << ")"
               << " previousRecon=" << FormatResourceState(reconstructedStates_[previousReconIndex_]) << "(" << FormatResourceStateHex(reconstructedStates_[previousReconIndex_]) << ")";
        log_.info(states.str());
    }

    videoBarriers_.Clear();
    transitionVideo("input texture", resource, currentState, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ);
    transitionVideo("bitstream buffer", bitstreamBuffer_.Get(), bitstreamState_, D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE);
    transitionVideo("encoder metadata buffer", encoderMetadataBuffer_.Get(), encoderMetadataState_, D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE);
    transitionVideo("resolved metadata buffer", resolvedMetadataBuffer_.Get(), resolvedMetadataState_, D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE);
    transitionVideo("current reconstructed texture", reconstructedPictures_[currentReconIndex_].Get(), reconstructedStates_[currentReconIndex_], D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE);

    if (!idr && hasReferenceFrame_) {
        transitionVideo("previous reconstructed texture", reconstructedPictures_[previousReconIndex_].Get(), reconstructedStates_[previousReconIndex_], D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ);
    }
    recordVideoBarrierPhase();
    if (!idr && hasReferenceFrame_) {
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
        h264Pic.idr_pic_id = h264CurrentIdrPicId;
        h264Pic.PictureOrderCountNumber = h264CurrentPoc;
        h264Pic.FrameDecodingOrderNumber = h264CurrentDecodingOrder;
        h264Pic.TemporalLayerIndex = 0;

        if (!idr && hasReferenceFrame_) {
            h264RefDesc.ReconstructedPictureResourceIndex = 0;
            h264RefDesc.IsLongTermReference = FALSE;
            h264RefDesc.LongTermPictureIdx = 0;
            h264RefDesc.PictureOrderCountNumber = h264ReferencePoc;
            h264RefDesc.FrameDecodingOrderNumber = h264ReferenceDecodingOrder;
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

    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream refs;
        refs << "ReferenceDiagnostic frame=" << frameIndex_
             << " type=" << (idr ? "IDR" : "P")
             << " referenceCount=" << referenceFrames.NumTexture2Ds
             << " refResource=" << FormatResourcePointer(refResource)
             << " refResourcePtrStorage=" << static_cast<const void*>(referenceFrames.ppTexture2Ds)
             << " refSubresourceStorage=" << static_cast<const void*>(referenceFrames.pSubresources)
             << " h264PicStorage=" << static_cast<const void*>(&h264Pic)
             << " h264List0Storage=" << static_cast<const void*>(h264Pic.pList0ReferenceFrames)
             << " h264RefDescStorage=" << static_cast<const void*>(h264Pic.pReferenceFramesReconPictureDescriptors)
             << " hevcPicStorage=" << static_cast<const void*>(&hevcPic)
             << " hevcList0Storage=" << static_cast<const void*>(hevcPic.pList0ReferenceFrames)
             << " hevcRefDescStorage=" << static_cast<const void*>(hevcPic.pReferenceFramesReconPictureDescriptors);
        log_.info(refs.str());
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
    input.CurrentFrameBitstreamMetadataSize = static_cast<UINT>(
        std::min<size_t>(writer_.pendingBitstreamMetadataSize(), static_cast<size_t>(std::numeric_limits<UINT>::max())));

    D3D12_VIDEO_ENCODER_ENCODEFRAME_OUTPUT_ARGUMENTS output = {};
    output.Bitstream.pBuffer = bitstreamBuffer_.Get();
    output.Bitstream.FrameStartOffset = 0;
    output.ReconstructedPicture.pReconstructedPicture = reconstructedPictures_[currentReconIndex_].Get();
    output.ReconstructedPicture.ReconstructedPictureSubresource = 0;
    output.EncoderOutputMetadata.pBuffer = encoderMetadataBuffer_.Get();
    output.EncoderOutputMetadata.Offset = 0;

    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream beforeEncode;
        beforeEncode << "Stage frame=" << frameIndex_
                     << " before EncodeFrame recording"
                     << " frameType=" << (idr ? "IDR" : "P")
                     << " CurrentFrameBitstreamMetadataSize=" << input.CurrentFrameBitstreamMetadataSize
                     << " outputBitstream=" << FormatResourcePointer(output.Bitstream.pBuffer)
                     << " outputRecon=" << FormatResourcePointer(output.ReconstructedPicture.pReconstructedPicture)
                     << " outputMetadata=" << FormatResourcePointer(output.EncoderOutputMetadata.pBuffer);
        log_.info(beforeEncode.str());
    }
    videoCommandList_->EncodeFrame(encoder_.Get(), encoderHeap_.Get(), &input, &output);
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream afterEncode;
        afterEncode << "Stage frame=" << frameIndex_ << " after EncodeFrame recording";
        log_.info(afterEncode.str());
    }

    videoBarriers_.Clear();
    transitionVideo("encoder metadata buffer", encoderMetadataBuffer_.Get(), D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ);
    recordVideoBarrierPhase();

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
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream beforeResolve;
        beforeResolve << "Stage frame=" << frameIndex_
                      << " before ResolveEncoderOutputMetadata recording"
                      << " hwMetadata=" << FormatResourcePointer(resolveInput.HWLayoutMetadata.pBuffer)
                      << " resolvedMetadata=" << FormatResourcePointer(resolveOutput.ResolvedLayoutMetadata.pBuffer);
        log_.info(beforeResolve.str());
    }
    videoCommandList_->ResolveEncoderOutputMetadata(&resolveInput, &resolveOutput);
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream afterResolve;
        afterResolve << "Stage frame=" << frameIndex_ << " after ResolveEncoderOutputMetadata recording";
        log_.info(afterResolve.str());
    }

    videoBarriers_.Clear();
    if (restoreInputState) {
        transitionVideo("input texture restore", resource, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ, currentState);
    }

    transitionVideo("bitstream buffer video queue handoff", bitstreamBuffer_.Get(), D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE, D3D12_RESOURCE_STATE_COMMON);
    transitionVideo("encoder metadata buffer video queue handoff", encoderMetadataBuffer_.Get(), D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ, D3D12_RESOURCE_STATE_COMMON);
    transitionVideo("resolved metadata buffer video queue handoff", resolvedMetadataBuffer_.Get(), D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE, D3D12_RESOURCE_STATE_COMMON);
    transitionVideo("current reconstructed texture video queue handoff", reconstructedPictures_[currentReconIndex_].Get(), D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE, D3D12_RESOURCE_STATE_COMMON);
    if (!idr && hasReferenceFrame_) {
        transitionVideo("previous reconstructed texture video queue handoff", reconstructedPictures_[previousReconIndex_].Get(), D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ, D3D12_RESOURCE_STATE_COMMON);
    }
    recordVideoBarrierPhase();

    bitstreamState_ = D3D12_RESOURCE_STATE_COMMON;
    encoderMetadataState_ = D3D12_RESOURCE_STATE_COMMON;
    resolvedMetadataState_ = D3D12_RESOURCE_STATE_COMMON;
    reconstructedStates_[currentReconIndex_] = D3D12_RESOURCE_STATE_COMMON;
    if (!idr && hasReferenceFrame_) {
        reconstructedStates_[previousReconIndex_] = D3D12_RESOURCE_STATE_COMMON;
    }
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream states;
        states << "FrameStateAfterBarriers frame=" << frameIndex_
               << " input=" << FormatResourceState(restoreInputState ? currentState : D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ)
               << "(" << FormatResourceStateHex(restoreInputState ? currentState : D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ) << ")"
               << " bitstream=" << FormatResourceState(bitstreamState_) << "(" << FormatResourceStateHex(bitstreamState_) << ")"
               << " encoderMetadata=" << FormatResourceState(encoderMetadataState_) << "(" << FormatResourceStateHex(encoderMetadataState_) << ")"
               << " resolvedMetadata=" << FormatResourceState(resolvedMetadataState_) << "(" << FormatResourceStateHex(resolvedMetadataState_) << ")"
               << " currentRecon=" << FormatResourceState(reconstructedStates_[currentReconIndex_]) << "(" << FormatResourceStateHex(reconstructedStates_[currentReconIndex_]) << ")"
               << " previousRecon=" << FormatResourceState(reconstructedStates_[previousReconIndex_]) << "(" << FormatResourceStateHex(reconstructedStates_[previousReconIndex_]) << ")";
        log_.info(states.str());
    }
}

void D3D12VideoEncodeBackend::signalVideoAndWaitOnCopy() {
    try {
        if (ShouldTraceFrame(frameIndex_)) {
            std::ostringstream beforeSignal;
            beforeSignal << "Stage frame=" << frameIndex_ << " before Video queue Signal";
            log_.info(beforeSignal.str());
        }
        const auto point = videoQueue_.SignalPoint();
        if (ShouldTraceFrame(frameIndex_)) {
            log_.info(FormatFenceProgress("Stage after Video queue Signal", point));
            std::ostringstream beforeWait;
            beforeWait << "Stage frame=" << frameIndex_ << " before Direct queue GpuWaitPoint";
            log_.info(beforeWait.str());
        }
        desc_.input.core->DirectQueue().GpuWaitPoint(point);
        if (ShouldTraceFrame(frameIndex_)) {
            std::ostringstream afterWait;
            afterWait << "Stage frame=" << frameIndex_ << " after Direct queue GpuWaitPoint";
            log_.info(afterWait.str());
        }
    } catch (const std::exception& e) {
        throw D3DVideoEncoderError(std::string("D3D12 Video Encode video/direct queue sync failed: ") + e.what());
    }
}

void D3D12VideoEncodeBackend::copyOutputsToReadback() {
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream beforeReset;
        beforeReset << "Stage frame=" << frameIndex_ << " before Direct copy allocator Reset";
        log_.info(beforeReset.str());
    }
    try {
        copyAllocator_.Reset();
    } catch (const std::exception& e) {
        throw D3DVideoEncoderError(std::string("copyAllocator Reset failed: ") + e.what());
    }
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream afterReset;
        afterReset << "Stage frame=" << frameIndex_ << " after Direct copy allocator Reset";
        log_.info(afterReset.str());
    }
    throw_hr(copyCommandList_->Reset(copyAllocator_.GetAllocator(), nullptr), "copyCommandList Reset");
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream afterCommandListReset;
        afterCommandListReset << "Stage frame=" << frameIndex_ << " after Direct copy command list Reset";
        log_.info(afterCommandListReset.str());
    }

    const auto bitstreamBeforeCopy = bitstreamState_;
    const auto resolvedMetadataBeforeCopy = resolvedMetadataState_;
    copyBarriers_.Clear();
    const bool bitstreamCopyTransition = transitionCopy(
        "bitstream buffer", bitstreamBuffer_.Get(), bitstreamState_, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const bool metadataCopyTransition = transitionCopy(
        "resolved metadata buffer", resolvedMetadataBuffer_.Get(), resolvedMetadataState_, D3D12_RESOURCE_STATE_COPY_SOURCE);
    recordCopyBarrierPhase();
    if (bitstreamCopyTransition) bitstreamState_ = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if (metadataCopyTransition) resolvedMetadataState_ = D3D12_RESOURCE_STATE_COPY_SOURCE;

    copyCommandList_->CopyBufferRegion(bitstreamReadback_.Get(), 0, bitstreamBuffer_.Get(), 0, bitstreamBufferSize_);
    copyCommandList_->CopyBufferRegion(resolvedMetadataReadback_.Get(), 0, resolvedMetadataBuffer_.Get(), 0, resolvedMetadataBufferSize_);
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream copyRecorded;
        copyRecorded << "Stage frame=" << frameIndex_
                     << " after Direct CopyBufferRegion recording"
                     << " bitstreamBytes=" << bitstreamBufferSize_
                     << " metadataBytes=" << resolvedMetadataBufferSize_;
        log_.info(copyRecorded.str());
    }

    const auto bitstreamAfterCopy = bitstreamState_;
    const auto resolvedMetadataAfterCopy = resolvedMetadataState_;
    copyBarriers_.Clear();
    const bool bitstreamRestoreTransition = transitionCopy(
        "bitstream buffer", bitstreamBuffer_.Get(), bitstreamState_, D3D12_RESOURCE_STATE_COMMON);
    const bool metadataRestoreTransition = transitionCopy(
        "resolved metadata buffer", resolvedMetadataBuffer_.Get(), resolvedMetadataState_, D3D12_RESOURCE_STATE_COMMON);
    recordCopyBarrierPhase();
    if (bitstreamRestoreTransition) bitstreamState_ = D3D12_RESOURCE_STATE_COMMON;
    if (metadataRestoreTransition) resolvedMetadataState_ = D3D12_RESOURCE_STATE_COMMON;

    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream bitstream;
        bitstream << "DirectCopyState frame=" << frameIndex_
                  << " resource=bitstream buffer"
                  << " beforeCopy=" << FormatResourceState(bitstreamBeforeCopy) << "(" << FormatResourceStateHex(bitstreamBeforeCopy) << ")"
                  << " duringCopy=COPY_SOURCE(" << FormatResourceStateHex(D3D12_RESOURCE_STATE_COPY_SOURCE) << ")"
                  << " afterCopy=" << FormatResourceState(bitstreamAfterCopy) << "(" << FormatResourceStateHex(bitstreamAfterCopy) << ")"
                  << " stateMember=" << FormatResourceState(bitstreamState_) << "(" << FormatResourceStateHex(bitstreamState_) << ")";
        log_.info(bitstream.str());

        std::ostringstream metadata;
        metadata << "DirectCopyState frame=" << frameIndex_
                 << " resource=resolved metadata buffer"
                 << " beforeCopy=" << FormatResourceState(resolvedMetadataBeforeCopy) << "(" << FormatResourceStateHex(resolvedMetadataBeforeCopy) << ")"
                 << " duringCopy=COPY_SOURCE(" << FormatResourceStateHex(D3D12_RESOURCE_STATE_COPY_SOURCE) << ")"
                 << " afterCopy=" << FormatResourceState(resolvedMetadataAfterCopy) << "(" << FormatResourceStateHex(resolvedMetadataAfterCopy) << ")"
                 << " stateMember=" << FormatResourceState(resolvedMetadataState_) << "(" << FormatResourceStateHex(resolvedMetadataState_) << ")";
        log_.info(metadata.str());
    }

    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream beforeClose;
        beforeClose << "Stage frame=" << frameIndex_ << " before Direct copy command list Close";
        log_.info(beforeClose.str());
    }
    throw_hr(copyCommandList_->Close(), "copyCommandList Close");
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream afterClose;
        afterClose << "Stage frame=" << frameIndex_ << " after Direct copy command list Close";
        log_.info(afterClose.str());
    }
    ID3D12CommandList* lists[] = { copyCommandList_.Get() };
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream beforeExecute;
        beforeExecute << "Stage frame=" << frameIndex_ << " before Direct copy ExecuteCommandLists";
        log_.info(beforeExecute.str());
    }
    desc_.input.core->DirectQueue().ExecuteCommandLists(1, lists);
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream afterExecute;
        afterExecute << "Stage frame=" << frameIndex_ << " after Direct copy ExecuteCommandLists";
        log_.info(afterExecute.str());
    }
}

void D3D12VideoEncodeBackend::waitForCopyQueue() {
    try {
        auto& directQueue = desc_.input.core->DirectQueue();
        if (ShouldTraceFrame(frameIndex_)) {
            std::ostringstream beforeSignal;
            beforeSignal << "Stage frame=" << frameIndex_ << " before Direct queue Signal";
            log_.info(beforeSignal.str());
        }
        const UINT64 fenceValue = directQueue.Signal();
        ID3D12Fence* fence = directQueue.Fence().Get();
        if (ShouldTraceFrame(frameIndex_)) {
            log_.info(FormatFenceProgress("Stage after Direct queue Signal", fenceValue, fence));
            std::ostringstream beforeWait;
            beforeWait << "Stage frame=" << frameIndex_ << " before Direct fence wait";
            log_.info(beforeWait.str());
        }
        directQueue.WaitForFenceValue(fenceValue);
        if (ShouldTraceFrame(frameIndex_)) {
            log_.info(FormatFenceProgress("Stage after Direct fence wait", fenceValue, fence));
        }
    } catch (const std::exception& e) {
        throw D3DVideoEncoderError(std::string("D3D12 Video Encode direct queue wait failed: ") + e.what());
    }
}

void D3D12VideoEncodeBackend::writeResolvedBitstream(int64_t timestamp100ns, int64_t duration100ns) {
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream beforeMap;
        beforeMap << "Stage frame=" << frameIndex_ << " before metadata MapRead";
        log_.info(beforeMap.str());
    }
    auto metadataRange = resolvedMetadataReadback_.MapRead(0, sizeof(D3D12_VIDEO_ENCODER_OUTPUT_METADATA));
    const auto* metadata = reinterpret_cast<const D3D12_VIDEO_ENCODER_OUTPUT_METADATA*>(metadataRange.Data());
    const D3D12_VIDEO_ENCODER_OUTPUT_METADATA metadataCopy = *metadata;
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream afterMap;
        afterMap << "Stage frame=" << frameIndex_
                 << " after metadata MapRead"
                 << " EncodeErrorFlags=0x" << std::hex << std::uppercase << metadataCopy.EncodeErrorFlags
                 << " EncodedBytes=" << std::dec << metadataCopy.EncodedBitstreamWrittenBytesCount;
        log_.info(afterMap.str());
    }

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

    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream beforeBitstreamMap;
        beforeBitstreamMap << "Stage frame=" << frameIndex_
                           << " before bitstream MapRead"
                           << " bytes=" << bytesToWrite;
        log_.info(beforeBitstreamMap.str());
    }
    auto bitstreamRange = bitstreamReadback_.MapRead(0, bytesToWrite);
    const auto* bitstream = reinterpret_cast<const uint8_t*>(bitstreamRange.Data());
    writer_.writeAccessUnit(bitstream, static_cast<size_t>(bytesToWrite), timestamp100ns, duration100ns);
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream afterWrite;
        afterWrite << "Stage frame=" << frameIndex_
                   << " after writer output"
                   << " bytes=" << bytesToWrite
                   << " timestamp100ns=" << timestamp100ns
                   << " duration100ns=" << duration100ns;
        log_.info(afterWrite.str());
    }
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
        if (currentState == D3D12_RESOURCE_STATE_COPY_DEST) {
            throw D3DVideoEncoderError(
                "D3D12VideoEncodeBackend input state " + FormatResourceState(currentState) +
                " cannot be used for native D3D12 Video Encode input cross-queue video encode handoff. "
                "Transition the resource to COMMON on the producer queue before calling write().");
        }
    }

    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream beforeReset;
        beforeReset << "Stage frame=" << frameIndex_ << " before Video allocator Reset";
        log_.info(beforeReset.str());
    }
    try {
        videoAllocator_.Reset();
    } catch (const std::exception& e) {
        throw D3DVideoEncoderError(std::string("videoAllocator Reset failed: ") + e.what());
    }
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream afterReset;
        afterReset << "Stage frame=" << frameIndex_ << " after Video allocator Reset";
        log_.info(afterReset.str());
    }
    throw_hr(videoCommandList_->Reset(videoAllocator_.GetAllocator()), "videoCommandList Reset");
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream afterCommandListReset;
        afterCommandListReset << "Stage frame=" << frameIndex_ << " after Video command list Reset";
        log_.info(afterCommandListReset.str());
    }
    recordEncodeFrame(encodeResource, encodeState, restoreEncodeResourceState);
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream beforeClose;
        beforeClose << "Stage frame=" << frameIndex_ << " before Video command list Close";
        log_.info(beforeClose.str());
    }
    throw_hr(videoCommandList_->Close(), "videoCommandList Close");
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream afterClose;
        afterClose << "Stage frame=" << frameIndex_ << " after Video command list Close";
        log_.info(afterClose.str());
    }

    ID3D12CommandList* lists[] = { videoCommandList_.Get() };
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream beforeExecute;
        beforeExecute << "Stage frame=" << frameIndex_ << " before Video ExecuteCommandLists";
        log_.info(beforeExecute.str());
    }
    videoQueue_.ExecuteCommandLists(1, lists);
    if (ShouldTraceFrame(frameIndex_)) {
        std::ostringstream afterExecute;
        afterExecute << "Stage frame=" << frameIndex_ << " after Video ExecuteCommandLists";
        log_.info(afterExecute.str());
    }

    signalVideoAndWaitOnCopy();
    copyOutputsToReadback();
    waitForCopyQueue();
    writeResolvedBitstream(timestamp100ns, duration100ns);

    if (useProcessing_) {
        convertedTextureState_ = D3D12_RESOURCE_STATE_COMMON;
        yuvToRgbaTextureState_ = D3D12_RESOURCE_STATE_COMMON;
    }

    if (desc_.codec == VideoCodec::H264) {
        const bool h264Idr = is_idr_frame(frameIndex_, desc_.gopLength) || !hasReferenceFrame_;
        const uint64_t h264FrameOffset = frameIndex_ - h264CurrentGopStartFrame_;
        const uint32_t h264FrameInGop = static_cast<uint32_t>(std::min<uint64_t>(h264FrameOffset, UINT32_MAX));
        const uint32_t h264CurrentDecodingOrder = h264Idr ? 0u : h264FrameInGop;
        const uint32_t h264CurrentPoc = h264Idr ? 0u : h264FrameInGop;

        h264PreviousReferenceDecodingOrder_ = h264CurrentDecodingOrder;
        h264PreviousReferencePoc_ = h264CurrentPoc;
        if (h264Idr) {
            h264NextIdrPicId_ = next_h264_idr_pic_id(h264NextIdrPicId_);
        }
    }
    hasReferenceFrame_ = true;
    ++frameIndex_;
}

void D3D12VideoEncodeBackend::flush() {
    if (writer_.isOpen()) {
        writer_.flush();
    }
}

void D3D12VideoEncodeBackend::waitForProcessingCompletion() {
    if (processingFenceValue_ == 0 || !desc_.input.core) return;
    const UINT64 fenceValue = processingFenceValue_;
    processingFenceValue_ = 0;
    desc_.input.core->DirectQueue().WaitForFenceValue(fenceValue);
}

void D3D12VideoEncodeBackend::destroyObjects() noexcept {
    videoBarriers_.Clear();
    copyBarriers_.Clear();
    processingFenceValue_ = 0;
    convertedTexture_ = {};
    yuvToRgbaTexture_ = {};
    resizedTexture_ = {};
    for (auto& recon : reconstructedPictures_) recon = {};
    resolvedMetadataReadback_ = {};
    resolvedMetadataBuffer_ = {};
    encoderMetadataBuffer_ = {};
    bitstreamReadback_ = {};
    bitstreamBuffer_ = {};
    copyCommandList_.Reset();
    copyAllocator_ = {};
    videoCommandList_.Reset();
    videoAllocator_ = {};
    videoQueue_ = {};
    encoderHeap_.Reset();
    encoder_.Reset();
    videoDevice3_.Reset();
    videoDevice_.Reset();
    bitstreamState_ = D3D12_RESOURCE_STATE_COMMON;
    encoderMetadataState_ = D3D12_RESOURCE_STATE_COMMON;
    resolvedMetadataState_ = D3D12_RESOURCE_STATE_COMMON;
    reconstructedStates_.fill(D3D12_RESOURCE_STATE_COMMON);
}

void D3D12VideoEncodeBackend::close() {
    std::exception_ptr pending;
    const auto captureFirst = [&pending](auto&& operation) noexcept {
        try {
            operation();
        } catch (...) {
            if (!pending) pending = std::current_exception();
        }
    };

    if (open_) captureFirst([this] { flush(); });
    captureFirst([this] { waitForProcessingCompletion(); });
    if (writer_.isOpen()) captureFirst([this] { writer_.close(); });

    open_ = false;
    destroyObjects();
    if (pending) {
        std::rethrow_exception(pending);
    }
}

} // namespace D3DVideoEncoderLib
