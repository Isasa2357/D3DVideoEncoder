#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace D3DVideoEncoderLib {

struct H264ParameterSetConfig {
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t profileIdc = 100;
    uint8_t constraintFlags = 0;
    uint8_t levelIdc = 52;
    uint32_t seqParameterSetId = 0;
    uint32_t picParameterSetId = 0;
    uint32_t chromaFormatIdc = 1;
    uint32_t bitDepthLumaMinus8 = 0;
    uint32_t bitDepthChromaMinus8 = 0;
    uint32_t log2MaxFrameNumMinus4 = 4;
    uint32_t picOrderCntType = 0;
    uint32_t log2MaxPicOrderCntLsbMinus4 = 4;
    uint32_t maxNumRefFrames = 2;
    bool entropyCodingModeFlag = false;
    bool bottomFieldPicOrderInFramePresentFlag = false;
    bool deblockingFilterControlPresentFlag = true;
    bool constrainedIntraPredFlag = false;
    bool transform8x8ModeFlag = false;
};

struct H264ParameterSets {
    std::vector<uint8_t> spsNal;
    std::vector<uint8_t> ppsNal;
    std::vector<uint8_t> spsAnnexB;
    std::vector<uint8_t> ppsAnnexB;
};

H264ParameterSets GenerateH264ParameterSets(const H264ParameterSetConfig& config);

} // namespace D3DVideoEncoderLib
