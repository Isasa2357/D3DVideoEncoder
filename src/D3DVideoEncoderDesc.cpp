#include <D3DVideoEncoder/D3DVideoEncoderDesc.hpp>

namespace D3DVideoEncoderLib {

namespace {
void CopyCommon(VideoEncoderCommonDesc& dst, const VideoEncoderCommonDesc& src) {
    dst.outputPath = src.outputPath;
    dst.width = src.width;
    dst.height = src.height;
    dst.frameRateNum = src.frameRateNum;
    dst.frameRateDen = src.frameRateDen;
    dst.backend = src.backend;
    dst.codec = src.codec;
    dst.internalFormat = src.internalFormat;
    dst.bitrate = src.bitrate;
    dst.rateControl = src.rateControl;
    dst.gopLength = src.gopLength;
    dst.bFrameCount = src.bFrameCount;
    dst.colorRange = src.colorRange;
    dst.colorMatrix = src.colorMatrix;
    dst.enableHardwareTransform = src.enableHardwareTransform;
    dst.useOnlyHardwareTransform = src.useOnlyHardwareTransform;
    dst.asyncMode = src.asyncMode;
    dst.queueDepth = src.queueDepth;
    dst.queueFullPolicy = src.queueFullPolicy;
    dst.enableDebugLog = src.enableDebugLog;
}
}

D3DVideoEncoderDesc ToLegacyDesc(const D3D11VideoEncoderDesc& desc) {
    D3DVideoEncoderDesc out;
    CopyCommon(out, desc);
    out.inputApi = D3DVideoInputApi::D3D11;
    out.d3d11 = desc.input;
    return out;
}

D3DVideoEncoderDesc ToLegacyDesc(const D3D12VideoEncoderDesc& desc) {
    D3DVideoEncoderDesc out;
    CopyCommon(out, desc);
    out.inputApi = D3DVideoInputApi::D3D12;
    out.d3d12 = desc.input;
    return out;
}

D3D11VideoEncoderDesc ToD3D11Desc(const D3DVideoEncoderDesc& desc) {
    D3D11VideoEncoderDesc out;
    CopyCommon(out, desc);
    out.input = desc.d3d11;
    return out;
}

D3D12VideoEncoderDesc ToD3D12Desc(const D3DVideoEncoderDesc& desc) {
    D3D12VideoEncoderDesc out;
    CopyCommon(out, desc);
    out.input = desc.d3d12;
    return out;
}

} // namespace D3DVideoEncoderLib
