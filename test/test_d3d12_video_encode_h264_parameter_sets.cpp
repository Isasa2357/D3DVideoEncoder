#include "backend/d3d12video/D3D12VideoEncodeH264ParameterSets.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace D3DVideoEncoderLib;

namespace {

void require_true(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

std::vector<std::pair<size_t, size_t>> find_annexb_nalus(const std::vector<uint8_t>& bytes) {
    std::vector<std::pair<size_t, size_t>> ranges;
    size_t i = 0;
    auto is3 = [&](size_t p) { return p + 3 <= bytes.size() && bytes[p] == 0 && bytes[p + 1] == 0 && bytes[p + 2] == 1; };
    auto is4 = [&](size_t p) { return p + 4 <= bytes.size() && bytes[p] == 0 && bytes[p + 1] == 0 && bytes[p + 2] == 0 && bytes[p + 3] == 1; };
    while (i + 3 <= bytes.size()) {
        size_t sc = bytes.size();
        size_t scLen = 0;
        for (; i + 3 <= bytes.size(); ++i) {
            if (is4(i)) { sc = i; scLen = 4; break; }
            if (is3(i)) { sc = i; scLen = 3; break; }
        }
        if (sc == bytes.size()) break;
        size_t nalStart = sc + scLen;
        i = nalStart;
        size_t next = bytes.size();
        for (; i + 3 <= bytes.size(); ++i) {
            if (is4(i) || is3(i)) {
                next = i;
                break;
            }
        }
        while (nalStart < next && bytes[nalStart] == 0) ++nalStart;
        if (next > nalStart) ranges.emplace_back(nalStart, next - nalStart);
    }
    return ranges;
}

std::vector<uint8_t> remove_emulation_prevention(const std::vector<uint8_t>& nal) {
    std::vector<uint8_t> rbsp;
    rbsp.reserve(nal.size());
    int zeroCount = 0;
    for (size_t i = 1; i < nal.size(); ++i) {
        const uint8_t byte = nal[i];
        if (zeroCount >= 2 && byte == 0x03) {
            zeroCount = 0;
            continue;
        }
        rbsp.push_back(byte);
        if (byte == 0) ++zeroCount;
        else zeroCount = 0;
    }
    return rbsp;
}

void require_no_forbidden_unescaped_sequence(const std::vector<uint8_t>& nal) {
    int zeroCount = 0;
    for (size_t i = 1; i < nal.size(); ++i) {
        const uint8_t byte = nal[i];
        if (zeroCount >= 2 && byte <= 0x02) {
            throw std::runtime_error("EBSP contains forbidden unescaped 00 00 00/01/02 sequence.");
        }
        if (byte == 0) ++zeroCount;
        else zeroCount = 0;
    }
}

class BitReader {
public:
    explicit BitReader(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}

    uint32_t readBit() {
        if (bitOffset_ >= bytes_.size() * 8) {
            throw std::runtime_error("BitReader overrun.");
        }
        const size_t byteOffset = bitOffset_ / 8;
        const uint32_t bitInByte = 7u - static_cast<uint32_t>(bitOffset_ % 8);
        ++bitOffset_;
        return (bytes_[byteOffset] >> bitInByte) & 1u;
    }

    uint32_t readBits(uint32_t count) {
        uint32_t value = 0;
        for (uint32_t i = 0; i < count; ++i) {
            value = (value << 1) | readBit();
        }
        return value;
    }

    uint32_t readUe() {
        uint32_t zeros = 0;
        while (readBit() == 0) {
            ++zeros;
            if (zeros > 31) throw std::runtime_error("Exp-Golomb code is too large.");
        }
        const uint32_t suffix = zeros == 0 ? 0 : readBits(zeros);
        return ((1u << zeros) - 1u) + suffix;
    }

    int32_t readSe() {
        const uint32_t codeNum = readUe();
        const int32_t magnitude = static_cast<int32_t>((codeNum + 1) / 2);
        return (codeNum & 1u) ? magnitude : -magnitude;
    }

private:
    std::vector<uint8_t> bytes_;
    size_t bitOffset_ = 0;
};

struct ParsedSps {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t spsId = 0;
    uint32_t profileIdc = 0;
    uint32_t levelIdc = 0;
    uint32_t chromaFormatIdc = 1;
    uint32_t maxNumRefFrames = 0;
};

struct ParsedPps {
    uint32_t ppsId = 0;
    uint32_t spsId = 0;
    bool entropyCodingModeFlag = false;
    bool deblockingFilterControlPresentFlag = false;
    bool transform8x8ModeFlag = false;
};

ParsedSps parse_sps(const std::vector<uint8_t>& nal) {
    require_true(!nal.empty() && (nal[0] & 0x1f) == 7, "Expected SPS NAL type 7.");
    BitReader r(remove_emulation_prevention(nal));
    ParsedSps sps;
    sps.profileIdc = r.readBits(8);
    r.readBits(8);
    sps.levelIdc = r.readBits(8);
    sps.spsId = r.readUe();
    sps.chromaFormatIdc = r.readUe();
    if (sps.chromaFormatIdc == 3) {
        r.readBit();
    }
    r.readUe(); // bit_depth_luma_minus8
    r.readUe(); // bit_depth_chroma_minus8
    r.readBit(); // qpprime_y_zero_transform_bypass_flag
    const bool seqScalingMatrixPresent = r.readBit() != 0;
    require_true(!seqScalingMatrixPresent, "Unexpected SPS scaling matrix.");
    r.readUe(); // log2_max_frame_num_minus4
    const uint32_t picOrderCntType = r.readUe();
    if (picOrderCntType == 0) {
        r.readUe();
    } else if (picOrderCntType == 1) {
        r.readBit();
        r.readSe();
        r.readSe();
        const uint32_t cycles = r.readUe();
            for (uint32_t i = 0; i < cycles; ++i) r.readSe();
    }
    sps.maxNumRefFrames = r.readUe();
    r.readBit();
    const uint32_t widthMbsMinus1 = r.readUe();
    const uint32_t heightMapUnitsMinus1 = r.readUe();
    const bool frameMbsOnly = r.readBit() != 0;
    if (!frameMbsOnly) {
        r.readBit();
    }
    r.readBit();
    uint32_t cropLeft = 0;
    uint32_t cropRight = 0;
    uint32_t cropTop = 0;
    uint32_t cropBottom = 0;
    if (r.readBit() != 0) {
        cropLeft = r.readUe();
        cropRight = r.readUe();
        cropTop = r.readUe();
        cropBottom = r.readUe();
    }

    const uint32_t codedWidth = (widthMbsMinus1 + 1) * 16;
    const uint32_t codedHeight = (heightMapUnitsMinus1 + 1) * 16 * (frameMbsOnly ? 1u : 2u);
    const uint32_t cropUnitX = 2;
    const uint32_t cropUnitY = frameMbsOnly ? 2u : 4u;
    sps.width = codedWidth - cropUnitX * (cropLeft + cropRight);
    sps.height = codedHeight - cropUnitY * (cropTop + cropBottom);
    return sps;
}

ParsedPps parse_pps(const std::vector<uint8_t>& nal) {
    require_true(!nal.empty() && (nal[0] & 0x1f) == 8, "Expected PPS NAL type 8.");
    BitReader r(remove_emulation_prevention(nal));
    ParsedPps pps;
    pps.ppsId = r.readUe();
    pps.spsId = r.readUe();
    pps.entropyCodingModeFlag = r.readBit() != 0;
    r.readBit();
    const uint32_t sliceGroups = r.readUe();
    require_true(sliceGroups == 0, "Unexpected slice group configuration.");
    r.readUe();
    r.readUe();
    r.readBit();
    r.readBits(2);
    r.readSe();
    r.readSe();
    r.readSe();
    pps.deblockingFilterControlPresentFlag = r.readBit() != 0;
    r.readBit();
    r.readBit();
    pps.transform8x8ModeFlag = r.readBit() != 0;
    return pps;
}

H264ParameterSetConfig make_config(uint32_t width, uint32_t height) {
    H264ParameterSetConfig config;
    config.width = width;
    config.height = height;
    config.profileIdc = 100;
    config.levelIdc = 52;
    config.log2MaxFrameNumMinus4 = 4;
    config.picOrderCntType = 0;
    config.log2MaxPicOrderCntLsbMinus4 = 4;
    config.maxNumRefFrames = 2;
    config.deblockingFilterControlPresentFlag = true;
    return config;
}

void test_case(uint32_t width, uint32_t height) {
    const auto config = make_config(width, height);
    const auto sets = GenerateH264ParameterSets(config);
    const auto setsAgain = GenerateH264ParameterSets(config);
    require_true(sets.spsAnnexB == setsAgain.spsAnnexB, "SPS output must be deterministic.");
    require_true(sets.ppsAnnexB == setsAgain.ppsAnnexB, "PPS output must be deterministic.");

    require_true(sets.spsNal.size() > 4, "SPS NAL is unexpectedly small.");
    require_true(sets.ppsNal.size() > 2, "PPS NAL is unexpectedly small.");
    require_true((sets.spsNal[0] & 0x1f) == 7, "SPS NAL type must be 7.");
    require_true((sets.ppsNal[0] & 0x1f) == 8, "PPS NAL type must be 8.");
    require_true(sets.spsAnnexB.size() >= 5 && sets.spsAnnexB[0] == 0 && sets.spsAnnexB[1] == 0 && sets.spsAnnexB[2] == 0 && sets.spsAnnexB[3] == 1, "SPS must use Annex B start code.");
    require_true(sets.ppsAnnexB.size() >= 5 && sets.ppsAnnexB[0] == 0 && sets.ppsAnnexB[1] == 0 && sets.ppsAnnexB[2] == 0 && sets.ppsAnnexB[3] == 1, "PPS must use Annex B start code.");
    require_no_forbidden_unescaped_sequence(sets.spsNal);
    require_no_forbidden_unescaped_sequence(sets.ppsNal);

    const auto sps = parse_sps(sets.spsNal);
    const auto pps = parse_pps(sets.ppsNal);
    require_true(sps.profileIdc == 100, "SPS profile_idc must be High profile.");
    require_true(sps.levelIdc == 52, "SPS level_idc must be 5.2.");
    require_true(sps.width == width, "Parsed SPS width does not match.");
    require_true(sps.height == height, "Parsed SPS height does not match.");
    require_true(sps.spsId == 0, "SPS id must be 0.");
    require_true(sps.maxNumRefFrames == config.maxNumRefFrames, "SPS max_num_ref_frames must match the configured reference-picture window.");
    require_true(pps.ppsId == 0, "PPS id must be 0.");
    require_true(pps.spsId == sps.spsId, "PPS must reference SPS id 0.");
    require_true(!pps.entropyCodingModeFlag, "PPS must match current CAVLC codec configuration.");
    require_true(pps.deblockingFilterControlPresentFlag, "PPS must expose deblocking filter control.");
    require_true(!pps.transform8x8ModeFlag, "PPS must match current non-adaptive-8x8 codec configuration.");

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), sets.spsAnnexB.begin(), sets.spsAnnexB.end());
    combined.insert(combined.end(), sets.ppsAnnexB.begin(), sets.ppsAnnexB.end());
    const auto nalus = find_annexb_nalus(combined);
    require_true(nalus.size() == 2, "Combined parameter sets must contain exactly SPS and PPS NAL units.");
}

} // namespace

int main() {
    try {
        test_case(640, 360);
        test_case(1280, 720);
        test_case(638, 358);
        std::cout << "D3D12 H.264 parameter-set tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "D3D12 H.264 parameter-set tests failed: " << e.what() << "\n";
        return 1;
    }
}
