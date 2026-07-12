#include "backend/d3d12video/D3D12VideoEncodeH264ParameterSets.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <vector>

namespace D3DVideoEncoderLib {
namespace {

class BitWriter {
public:
    void writeBit(bool bit) {
        currentByte_ = static_cast<uint8_t>((currentByte_ << 1) | (bit ? 1 : 0));
        ++bitCount_;
        if (bitCount_ == 8) {
            bytes_.push_back(currentByte_);
            currentByte_ = 0;
            bitCount_ = 0;
        }
    }

    void writeBits(uint32_t value, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            const uint32_t shift = count - i - 1;
            writeBit(((value >> shift) & 1u) != 0);
        }
    }

    void writeUe(uint32_t value) {
        const uint32_t codeNum = value + 1;
        uint32_t bits = 0;
        for (uint32_t v = codeNum; v != 0; v >>= 1) {
            ++bits;
        }
        for (uint32_t i = 1; i < bits; ++i) {
            writeBit(false);
        }
        writeBits(codeNum, bits);
    }

    void writeSe(int32_t value) {
        const uint32_t codeNum = value <= 0
            ? static_cast<uint32_t>(-value * 2)
            : static_cast<uint32_t>(value * 2 - 1);
        writeUe(codeNum);
    }

    std::vector<uint8_t> finishRbsp() {
        writeBit(true);
        while (bitCount_ != 0) {
            writeBit(false);
        }
        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
    uint8_t currentByte_ = 0;
    uint32_t bitCount_ = 0;
};

std::vector<uint8_t> rbspToEbsp(const std::vector<uint8_t>& rbsp) {
    std::vector<uint8_t> out;
    out.reserve(rbsp.size());
    uint32_t zeroCount = 0;
    for (uint8_t byte : rbsp) {
        if (zeroCount >= 2 && byte <= 0x03) {
            out.push_back(0x03);
            zeroCount = 0;
        }
        out.push_back(byte);
        if (byte == 0) {
            ++zeroCount;
        } else {
            zeroCount = 0;
        }
    }
    return out;
}

void appendAnnexB(std::vector<uint8_t>& out, const std::vector<uint8_t>& nal) {
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(1);
    out.insert(out.end(), nal.begin(), nal.end());
}

uint32_t ceilDiv(uint32_t value, uint32_t divisor) {
    return (value + divisor - 1) / divisor;
}

void validateConfig(const H264ParameterSetConfig& config) {
    if (config.width == 0 || config.height == 0) {
        throw D3DVideoEncoderError("H.264 parameter-set generation requires nonzero width and height.");
    }
    if (config.chromaFormatIdc != 1) {
        throw D3DVideoEncoderError("H.264 parameter-set generation currently supports 4:2:0 only.");
    }
    if ((config.width & 1u) != 0 || (config.height & 1u) != 0) {
        throw D3DVideoEncoderError("H.264 4:2:0 parameter-set generation requires even width and height.");
    }
    if (config.bitDepthLumaMinus8 != 0 || config.bitDepthChromaMinus8 != 0) {
        throw D3DVideoEncoderError("H.264 parameter-set generation currently supports 8-bit NV12 only.");
    }
}

std::vector<uint8_t> makeSpsNal(const H264ParameterSetConfig& config) {
    BitWriter w;
    w.writeBits(config.profileIdc, 8);
    w.writeBits(config.constraintFlags, 8);
    w.writeBits(config.levelIdc, 8);
    w.writeUe(config.seqParameterSetId);

    w.writeUe(config.chromaFormatIdc);
    w.writeUe(config.bitDepthLumaMinus8);
    w.writeUe(config.bitDepthChromaMinus8);
    w.writeBit(false); // qpprime_y_zero_transform_bypass_flag
    w.writeBit(false); // seq_scaling_matrix_present_flag

    w.writeUe(config.log2MaxFrameNumMinus4);
    w.writeUe(config.picOrderCntType);
    if (config.picOrderCntType == 0) {
        w.writeUe(config.log2MaxPicOrderCntLsbMinus4);
    } else if (config.picOrderCntType == 1) {
        w.writeBit(false);
        w.writeSe(0);
        w.writeSe(0);
        w.writeUe(0);
    }

    w.writeUe(config.maxNumRefFrames);
    w.writeBit(false); // gaps_in_frame_num_value_allowed_flag

    const uint32_t picWidthInMbs = ceilDiv(config.width, 16);
    const uint32_t picHeightInMapUnits = ceilDiv(config.height, 16);
    w.writeUe(picWidthInMbs - 1);
    w.writeUe(picHeightInMapUnits - 1);
    w.writeBit(true);  // frame_mbs_only_flag
    w.writeBit(true);  // direct_8x8_inference_flag

    const uint32_t codedWidth = picWidthInMbs * 16;
    const uint32_t codedHeight = picHeightInMapUnits * 16;
    const uint32_t cropRight = (codedWidth - config.width) / 2;
    const uint32_t cropBottom = (codedHeight - config.height) / 2;
    const bool hasCrop = cropRight != 0 || cropBottom != 0;
    w.writeBit(hasCrop);
    if (hasCrop) {
        w.writeUe(0);
        w.writeUe(cropRight);
        w.writeUe(0);
        w.writeUe(cropBottom);
    }

    w.writeBit(false); // vui_parameters_present_flag

    std::vector<uint8_t> nal;
    nal.push_back(0x67);
    const auto rbsp = w.finishRbsp();
    const auto ebsp = rbspToEbsp(rbsp);
    nal.insert(nal.end(), ebsp.begin(), ebsp.end());
    return nal;
}

std::vector<uint8_t> makePpsNal(const H264ParameterSetConfig& config) {
    BitWriter w;
    w.writeUe(config.picParameterSetId);
    w.writeUe(config.seqParameterSetId);
    w.writeBit(config.entropyCodingModeFlag);
    w.writeBit(config.bottomFieldPicOrderInFramePresentFlag);
    w.writeUe(0); // num_slice_groups_minus1
    w.writeUe(0); // num_ref_idx_l0_default_active_minus1
    w.writeUe(0); // num_ref_idx_l1_default_active_minus1
    w.writeBit(false); // weighted_pred_flag
    w.writeBits(0, 2);  // weighted_bipred_idc
    w.writeSe(0);       // pic_init_qp_minus26
    w.writeSe(0);       // pic_init_qs_minus26
    w.writeSe(0);       // chroma_qp_index_offset
    w.writeBit(config.deblockingFilterControlPresentFlag);
    w.writeBit(config.constrainedIntraPredFlag);
    w.writeBit(false);  // redundant_pic_cnt_present_flag
    w.writeBit(config.transform8x8ModeFlag);
    w.writeBit(false);  // pic_scaling_matrix_present_flag
    w.writeSe(0);       // second_chroma_qp_index_offset

    std::vector<uint8_t> nal;
    nal.push_back(0x68);
    const auto rbsp = w.finishRbsp();
    const auto ebsp = rbspToEbsp(rbsp);
    nal.insert(nal.end(), ebsp.begin(), ebsp.end());
    return nal;
}

} // namespace

H264ParameterSets GenerateH264ParameterSets(const H264ParameterSetConfig& config) {
    validateConfig(config);

    H264ParameterSets sets;
    sets.spsNal = makeSpsNal(config);
    sets.ppsNal = makePpsNal(config);
    appendAnnexB(sets.spsAnnexB, sets.spsNal);
    appendAnnexB(sets.ppsAnnexB, sets.ppsNal);
    return sets;
}

} // namespace D3DVideoEncoderLib
