#pragma once

#include <D3DVideoEncoder/D3DVideoEncoderTypes.hpp>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace D3DVideoEncoderLib {

class NvencOutputMuxer {
public:
    NvencOutputMuxer() = default;
    ~NvencOutputMuxer();

    NvencOutputMuxer(const NvencOutputMuxer&) = delete;
    NvencOutputMuxer& operator=(const NvencOutputMuxer&) = delete;

    void open(const std::wstring& outputPath, uint32_t width, uint32_t height, uint32_t frameRateNum, uint32_t frameRateDen, VideoCodec codec);
    void writeAccessUnit(const uint8_t* data, size_t size, int64_t timestamp100ns, int64_t duration100ns);
    void flush();
    void close();

    bool isOpen() const noexcept { return opened_; }
    bool isContainerMux() const noexcept;

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
    void writeMp4();
    void writeMkv();

    std::wstring outputPath_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t frameRateNum_ = 60;
    uint32_t frameRateDen_ = 1;
    VideoCodec codec_ = VideoCodec::H264;
    Container container_ = Container::Elementary;
    std::ofstream output_;
    bool opened_ = false;

    std::vector<Sample> samples_;
    std::vector<uint8_t> avcSps_;
    std::vector<uint8_t> avcPps_;
    std::vector<uint8_t> hevcVps_;
    std::vector<uint8_t> hevcSps_;
    std::vector<uint8_t> hevcPps_;
};

} // namespace D3DVideoEncoderLib
