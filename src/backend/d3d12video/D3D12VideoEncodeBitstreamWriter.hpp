#pragma once

#include "backend/d3d12video/D3D12VideoEncodeH264ParameterSets.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderTypes.hpp>

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace D3DVideoEncoderLib {

class D3D12VideoEncodeBitstreamWriter {
public:
    D3D12VideoEncodeBitstreamWriter() = default;
    ~D3D12VideoEncodeBitstreamWriter();

    D3D12VideoEncodeBitstreamWriter(const D3D12VideoEncodeBitstreamWriter&) = delete;
    D3D12VideoEncodeBitstreamWriter& operator=(const D3D12VideoEncodeBitstreamWriter&) = delete;

    void open(const std::wstring& outputPath);
    void open(
        const std::wstring& outputPath,
        uint32_t width,
        uint32_t height,
        uint32_t frameRateNum,
        uint32_t frameRateDen,
        VideoCodec codec);
    void writeAccessUnit(const uint8_t* data, size_t size, int64_t timestamp100ns, int64_t duration100ns);
    void flush();
    void close();
    void configureH264ParameterSets(const H264ParameterSetConfig& config);

    bool isOpen() const noexcept { return opened_; }
    bool isContainerMux() const noexcept;
    uint64_t bytesWritten() const noexcept { return bytesWritten_; }
    size_t pendingBitstreamMetadataSize() const noexcept;

private:
    enum class Container {
        Elementary,
        Mp4,
        Mkv,
    };

    struct Sample {
        std::vector<uint8_t> annexB;
        std::vector<uint8_t> lengthPrefixed;
        int64_t timestamp100ns = 0;
        int64_t duration100ns = 0;
        bool keyFrame = false;
    };

    Container chooseContainer(const std::wstring& outputPath) const;
    void parseParameterSetsAndSample(Sample& sample);
    void writeElementary(const uint8_t* data, size_t size);
    void writeElementaryH264(const uint8_t* data, size_t size);
    void writeMp4();
    void writeMkv();

    std::filesystem::path path_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t frameRateNum_ = 60;
    uint32_t frameRateDen_ = 1;
    VideoCodec codec_ = VideoCodec::H264;
    Container container_ = Container::Elementary;
    std::ofstream stream_;
    bool opened_ = false;
    uint64_t bytesWritten_ = 0;

    std::vector<Sample> samples_;
    std::vector<uint8_t> avcSps_;
    std::vector<uint8_t> avcPps_;
    std::vector<uint8_t> generatedAvcSpsAnnexB_;
    std::vector<uint8_t> generatedAvcPpsAnnexB_;
    std::vector<uint8_t> generatedAvcSps_;
    std::vector<uint8_t> generatedAvcPps_;
    bool h264ParameterSetsEmitted_ = false;
    std::vector<uint8_t> hevcVps_;
    std::vector<uint8_t> hevcSps_;
    std::vector<uint8_t> hevcPps_;
};

} // namespace D3DVideoEncoderLib
