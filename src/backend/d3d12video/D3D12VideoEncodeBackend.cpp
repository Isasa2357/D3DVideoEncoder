#include "backend/d3d12video/D3D12VideoEncodeBackend.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

#include <d3d12video.h>
#include <Windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <sstream>
#include <string>

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

D3D12_RESOURCE_DESC nv12_texture_desc(uint32_t width, uint32_t height) noexcept {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_NV12;
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

struct H264ProfileStorage {
    D3D12_VIDEO_ENCODER_PROFILE_H264 profile = D3D12_VIDEO_ENCODER_PROFILE_H264_HIGH;
    D3D12_VIDEO_ENCODER_PROFILE_DESC desc = {};

    H264ProfileStorage() {
        desc.DataSize = sizeof(profile);
        desc.pH264Profile = &profile;
    }
};

struct H264LevelStorage {
    D3D12_VIDEO_ENCODER_LEVELS_H264 level = D3D12_VIDEO_ENCODER_LEVELS_H264_52;
    D3D12_VIDEO_ENCODER_LEVEL_SETTING desc = {};

    H264LevelStorage() {
        desc.DataSize = sizeof(level);
        desc.pH264LevelSetting = &level;
    }
};

struct H264CodecConfigStorage {
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264 h264 = {};
    D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION desc = {};

    H264CodecConfigStorage() {
        h264.ConfigurationFlags = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_FLAG_NONE;
        h264.DirectModeConfig = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_DIRECT_MODES_DISABLED;
        h264.DisableDeblockingFilterConfig = D3D12_VIDEO_ENCODER_CODEC_CONFIGURATION_H264_SLICES_DEBLOCKING_MODE_0_ALL_LUMA_CHROMA_SLICE_BLOCK_EDGES_ALWAYS_FILTERED;
        desc.DataSize = sizeof(h264);
        desc.pH264Config = &h264;
    }
};

struct H264GopStorage {
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE_H264 h264 = {};
    D3D12_VIDEO_ENCODER_SEQUENCE_GOP_STRUCTURE desc = {};

    explicit H264GopStorage(uint32_t gopLength) {
        h264.GOPLength = std::max<uint32_t>(gopLength, 1);
        h264.PPicturePeriod = 1;
        h264.pic_order_cnt_type = 0;
        h264.log2_max_frame_num_minus4 = 4;
        h264.log2_max_pic_order_cnt_lsb_minus4 = 4;
        desc.DataSize = sizeof(h264);
        desc.pH264GroupOfPictures = &h264;
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
            cbr.MaxQP = 51;
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

void D3D12VideoEncodeBackend::initialize(const D3D12VideoEncoderDesc& desc) {
    desc_ = desc;
    validateDesc();
    queryVideoDevice();
    queryEncodeSupport();
    queryResourceRequirements();
    createEncoderObjects();
    createQueuesAndCommands();
    createBuffers();
    createReconstructedPictures();
    createFences();
    writer_.open(desc_.outputPath);

    open_ = true;
    log_.info("D3D12VideoEncodeBackend Phase 2 initialized: H.264/NV12 elementary stream output");
}

void D3D12VideoEncodeBackend::validateDesc() const {
    if (!desc_.input.core) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend requires desc.input.core.");
    }
    if (desc_.codec != VideoCodec::H264) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend Phase 2 supports only H.264.");
    }
    if (desc_.internalFormat != VideoPixelFormat::NV12) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend Phase 2 supports only internalFormat=NV12.");
    }
    if (desc_.input.inputFormat != DXGI_FORMAT_NV12) {
        throw D3DVideoEncoderError(
            "D3D12VideoEncodeBackend Phase 2 requires direct NV12 input. "
            "RGB input through D3D12Processing will be connected after the native H.264/NV12 path is stable.");
    }
    if (desc_.asyncMode) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend Phase 2 is synchronous only. Async will be enabled after the native path is stable.");
    }
    if (desc_.bFrameCount != 0) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend Phase 2 requires bFrameCount=0.");
    }
    if ((desc_.width % 2) != 0 || (desc_.height % 2) != 0 || desc_.width == 0 || desc_.height == 0) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBackend Phase 2 requires non-zero even width/height.");
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

    H264ProfileStorage profile;
    H264CodecConfigStorage codecConfig;
    H264GopStorage gop(desc_.gopLength);
    RateControlStorage rateControl(desc_, capability_.cbrSupported);
    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC resolution = { desc_.width, desc_.height };
    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOLUTION_SUPPORT_LIMITS resolutionLimits = {};

    D3D12_FEATURE_DATA_VIDEO_ENCODER_SUPPORT support = {};
    support.NodeIndex = 0;
    support.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
    support.InputFormat = DXGI_FORMAT_NV12;
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
    H264ProfileStorage profile;

    D3D12_FEATURE_DATA_VIDEO_ENCODER_RESOURCE_REQUIREMENTS req = {};
    req.NodeIndex = 0;
    req.Codec = D3D12_VIDEO_ENCODER_CODEC_H264;
    req.Profile = profile.desc;
    req.InputFormat = DXGI_FORMAT_NV12;
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
    const uint64_t roughFrameBytes = framePixels * 4ull + 1024ull * 1024ull;
    const uint64_t roughRateBytes = (static_cast<uint64_t>(desc_.bitrate) / std::max<uint32_t>(desc_.frameRateNum, 1u)) + 1024ull * 1024ull;
    bitstreamBufferSize_ = align_up_u64(std::max<uint64_t>(roughFrameBytes, roughRateBytes), bitstreamAlignment_);
}

void D3D12VideoEncodeBackend::createEncoderObjects() {
    H264ProfileStorage profile;
    H264LevelStorage level;
    H264CodecConfigStorage codecConfig;
    D3D12_VIDEO_ENCODER_PICTURE_RESOLUTION_DESC resolution = { desc_.width, desc_.height };

    D3D12_VIDEO_ENCODER_DESC encoderDesc = {};
    encoderDesc.NodeMask = 0;
    encoderDesc.Flags = D3D12_VIDEO_ENCODER_FLAG_NONE;
    encoderDesc.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_H264;
    encoderDesc.EncodeProfile = profile.desc;
    encoderDesc.InputFormat = DXGI_FORMAT_NV12;
    encoderDesc.CodecConfiguration = codecConfig.desc;
    encoderDesc.MaxMotionEstimationPrecision = D3D12_VIDEO_ENCODER_MOTION_ESTIMATION_PRECISION_MODE_FULL_PIXEL;

    throw_hr(videoDevice3_->CreateVideoEncoder(&encoderDesc, IID_PPV_ARGS(&encoder_)), "CreateVideoEncoder");

    D3D12_VIDEO_ENCODER_HEAP_DESC heapDesc = {};
    heapDesc.NodeMask = 0;
    heapDesc.Flags = D3D12_VIDEO_ENCODER_HEAP_FLAG_NONE;
    heapDesc.EncodeCodec = D3D12_VIDEO_ENCODER_CODEC_H264;
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
    const auto textureDesc = nv12_texture_desc(desc_.width, desc_.height);
    for (auto& recon : reconstructedPictures_) {
        throw_hr(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &textureDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&recon)), "Create reconstructed NV12 texture");
    }
}

void D3D12VideoEncodeBackend::createFences() {
    ID3D12Device* device = desc_.input.core->GetDevice();
    throw_hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&videoFence_)), "Create video fence");
    throw_hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copyFence_)), "Create copy fence");
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

    H264GopStorage gop(desc_.gopLength);
    RateControlStorage rateControl(desc_, capability_.cbrSupported);

    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264 h264Pic = {};
    h264Pic.Flags = D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA_H264_FLAG_NONE;
    h264Pic.FrameType = idr ? D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_IDR_FRAME : D3D12_VIDEO_ENCODER_FRAME_TYPE_H264_P_FRAME;
    h264Pic.pic_parameter_set_id = 0;
    h264Pic.idr_pic_id = static_cast<UINT>(frameIndex_ & 0xFFFFu);
    h264Pic.PictureOrderCountNumber = static_cast<UINT>(frameIndex_);
    h264Pic.FrameDecodingOrderNumber = static_cast<UINT>(frameIndex_);
    h264Pic.TemporalLayerIndex = 0;

    UINT list0Index = 0;
    D3D12_VIDEO_ENCODER_REFERENCE_PICTURE_DESCRIPTOR_H264 refDesc = {};
    ID3D12Resource* refResource = nullptr;
    UINT refSubresource = 0;

    if (!idr && hasReferenceFrame_) {
        refDesc.ReconstructedPictureResourceIndex = 0;
        refDesc.IsLongTermReference = FALSE;
        refDesc.LongTermPictureIdx = 0;
        refDesc.PictureOrderCountNumber = static_cast<UINT>(frameIndex_ - 1);
        refDesc.FrameDecodingOrderNumber = static_cast<UINT>(frameIndex_ - 1);
        refDesc.TemporalLayerIndex = 0;

        h264Pic.List0ReferenceFramesCount = 1;
        h264Pic.pList0ReferenceFrames = &list0Index;
        h264Pic.ReferenceFramesReconPictureDescriptorsCount = 1;
        h264Pic.pReferenceFramesReconPictureDescriptors = &refDesc;

        refResource = reconstructedPictures_[previousReconIndex_].Get();
    }

    D3D12_VIDEO_ENCODER_PICTURE_CONTROL_CODEC_DATA picCodecData = {};
    picCodecData.DataSize = sizeof(h264Pic);
    picCodecData.pH264PicData = &h264Pic;

    D3D12_VIDEO_ENCODE_REFERENCE_FRAMES referenceFrames = {};
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

    H264ProfileStorage profile;
    D3D12_VIDEO_ENCODER_RESOLVE_METADATA_INPUT_ARGUMENTS resolveInput = {};
    resolveInput.EncoderCodec = D3D12_VIDEO_ENCODER_CODEC_H264;
    resolveInput.EncoderProfile = profile.desc;
    resolveInput.EncoderInputFormat = DXGI_FORMAT_NV12;
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

    throw_hr(videoAllocator_->Reset(), "videoAllocator Reset");
    throw_hr(videoCommandList_->Reset(videoAllocator_.Get()), "videoCommandList Reset");
    recordEncodeFrame(resource, currentState, desc_.input.restoreStateAfterEncode);
    throw_hr(videoCommandList_->Close(), "videoCommandList Close");

    ID3D12CommandList* lists[] = { videoCommandList_.Get() };
    videoQueue_->ExecuteCommandLists(1, lists);

    signalVideoAndWaitOnCopy();
    copyOutputsToReadback();
    waitForCopyQueue();
    writeResolvedBitstream(timestamp100ns, duration100ns);

    hasReferenceFrame_ = true;
    ++frameIndex_;
}

void D3D12VideoEncodeBackend::flush() {
    if (writer_.isOpen()) {
        writer_.flush();
    }
}

void D3D12VideoEncodeBackend::destroyObjects() noexcept {
    writer_.close();
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
