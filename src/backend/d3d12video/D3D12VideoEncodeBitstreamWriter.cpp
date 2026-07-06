#include "backend/d3d12video/D3D12VideoEncodeBitstreamWriter.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <sstream>

namespace D3DVideoEncoderLib {
namespace {

constexpr uint32_t kTimeScale100ns = 10'000'000;

std::wstring lower_extension(const std::filesystem::path& path) {
    std::wstring ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return ext;
}

void put_u8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void put_u16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
void put_u24(std::vector<uint8_t>& b, uint32_t v) { b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
void put_u32(std::vector<uint8_t>& b, uint32_t v) { b.push_back(uint8_t(v >> 24)); b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
void put_u64(std::vector<uint8_t>& b, uint64_t v) { put_u32(b, uint32_t(v >> 32)); put_u32(b, uint32_t(v)); }
void put_i16(std::vector<uint8_t>& b, int16_t v) { put_u16(b, static_cast<uint16_t>(v)); }

void put_type(std::vector<uint8_t>& b, const char (&type)[5]) { b.insert(b.end(), type, type + 4); }

void append(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) {
    dst.insert(dst.end(), src.begin(), src.end());
}

std::vector<uint8_t> box(const char (&type)[5], const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> b;
    const uint64_t size = 8ull + payload.size();
    if (size > 0xffffffffull) {
        throw D3DVideoEncoderError("MP4 box is too large for the D3D12 Video Encode minimal muxer.");
    }
    put_u32(b, static_cast<uint32_t>(size));
    put_type(b, type);
    append(b, payload);
    return b;
}

std::vector<uint8_t> full_box(const char (&type)[5], uint8_t version, uint32_t flags, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> p;
    put_u8(p, version);
    put_u24(p, flags);
    append(p, payload);
    return box(type, p);
}

bool is_start_code3(const uint8_t* p) noexcept { return p[0] == 0 && p[1] == 0 && p[2] == 1; }
bool is_start_code4(const uint8_t* p) noexcept { return p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1; }

std::vector<std::pair<size_t, size_t>> find_annexb_nalus(const std::vector<uint8_t>& bytes) {
    std::vector<std::pair<size_t, size_t>> ranges;
    size_t i = 0;
    while (i + 3 <= bytes.size()) {
        size_t sc = std::string::npos;
        size_t scLen = 0;
        for (; i + 3 <= bytes.size(); ++i) {
            if (i + 4 <= bytes.size() && is_start_code4(bytes.data() + i)) { sc = i; scLen = 4; break; }
            if (is_start_code3(bytes.data() + i)) { sc = i; scLen = 3; break; }
        }
        if (sc == std::string::npos) break;
        size_t nalStart = sc + scLen;
        i = nalStart;
        size_t next = bytes.size();
        for (; i + 3 <= bytes.size(); ++i) {
            if ((i + 4 <= bytes.size() && is_start_code4(bytes.data() + i)) || is_start_code3(bytes.data() + i)) { next = i; break; }
        }
        while (nalStart < next && bytes[nalStart] == 0) ++nalStart;
        if (next > nalStart) ranges.emplace_back(nalStart, next - nalStart);
    }
    if (ranges.empty() && !bytes.empty()) ranges.emplace_back(0, bytes.size());
    return ranges;
}

std::vector<uint8_t> to_length_prefixed(const std::vector<uint8_t>& annexB) {
    std::vector<uint8_t> out;
    for (const auto& r : find_annexb_nalus(annexB)) {
        put_u32(out, static_cast<uint32_t>(r.second));
        out.insert(out.end(), annexB.begin() + static_cast<std::ptrdiff_t>(r.first), annexB.begin() + static_cast<std::ptrdiff_t>(r.first + r.second));
    }
    return out;
}

std::vector<uint8_t> make_avcc(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps) {
    if (sps.size() < 4 || pps.empty()) {
        throw D3DVideoEncoderError(
            "D3D12 Video Encode MP4/MKV mux requires H.264 SPS/PPS in the encoded bitstream. "
            "If your driver does not emit SPS/PPS, use .h264 output until parameter-set injection is implemented.");
    }
    std::vector<uint8_t> p;
    put_u8(p, 1);
    put_u8(p, sps[1]);
    put_u8(p, sps[2]);
    put_u8(p, sps[3]);
    put_u8(p, 0xff); // lengthSizeMinusOne = 3
    put_u8(p, 0xe1); // one SPS
    put_u16(p, static_cast<uint16_t>(sps.size()));
    p.insert(p.end(), sps.begin(), sps.end());
    put_u8(p, 1); // one PPS
    put_u16(p, static_cast<uint16_t>(pps.size()));
    p.insert(p.end(), pps.begin(), pps.end());
    return p;
}

std::vector<uint8_t> make_ftyp() {
    std::vector<uint8_t> p;
    put_type(p, "isom");
    put_u32(p, 0x200);
    put_type(p, "isom");
    put_type(p, "iso2");
    put_type(p, "mp41");
    put_type(p, "avc1");
    return box("ftyp", p);
}

std::vector<uint8_t> make_mvhd(uint64_t duration) {
    std::vector<uint8_t> p;
    put_u32(p, 0); put_u32(p, 0); put_u32(p, kTimeScale100ns); put_u32(p, static_cast<uint32_t>(duration));
    put_u32(p, 0x00010000); put_u16(p, 0x0100); put_u16(p, 0); put_u32(p, 0); put_u32(p, 0);
    const uint32_t matrix[9] = {0x00010000,0,0,0,0x00010000,0,0,0,0x40000000};
    for (uint32_t v : matrix) put_u32(p, v);
    for (int i = 0; i < 6; ++i) put_u32(p, 0);
    put_u32(p, 2);
    return full_box("mvhd", 0, 0, p);
}

std::vector<uint8_t> make_tkhd(uint32_t width, uint32_t height, uint64_t duration) {
    std::vector<uint8_t> p;
    put_u32(p,0); put_u32(p,0); put_u32(p,1); put_u32(p,0); put_u32(p,static_cast<uint32_t>(duration));
    put_u32(p,0); put_u32(p,0); put_u16(p,0); put_u16(p,0); put_u16(p,0); put_u16(p,0);
    const uint32_t matrix[9] = {0x00010000,0,0,0,0x00010000,0,0,0,0x40000000};
    for (uint32_t v : matrix) put_u32(p, v);
    put_u32(p, width << 16); put_u32(p, height << 16);
    return full_box("tkhd", 0, 0x000007, p);
}

std::vector<uint8_t> make_mdhd(uint64_t duration) {
    std::vector<uint8_t> p;
    put_u32(p,0); put_u32(p,0); put_u32(p,kTimeScale100ns); put_u32(p,static_cast<uint32_t>(duration));
    put_u16(p,0x55c4); put_u16(p,0);
    return full_box("mdhd",0,0,p);
}

std::vector<uint8_t> make_hdlr() {
    std::vector<uint8_t> p;
    put_u32(p,0); put_type(p,"vide"); put_u32(p,0); put_u32(p,0); put_u32(p,0);
    const char name[] = "VideoHandler";
    p.insert(p.end(), name, name + sizeof(name));
    return full_box("hdlr",0,0,p);
}

std::vector<uint8_t> make_video_sample_entry(uint32_t width, uint32_t height, const std::vector<uint8_t>& configBox) {
    std::vector<uint8_t> p;
    p.resize(6,0); put_u16(p,1); // reserved + data_reference_index
    put_u16(p,0); put_u16(p,0); put_u32(p,0); put_u32(p,0); put_u32(p,0);
    put_u16(p,static_cast<uint16_t>(width)); put_u16(p,static_cast<uint16_t>(height));
    put_u32(p,0x00480000); put_u32(p,0x00480000); put_u32(p,0); put_u16(p,1);
    p.push_back(0); p.resize(p.size()+31,0); put_u16(p,0x18); put_u16(p,0xffff);
    append(p, configBox);
    return box("avc1", p);
}

std::vector<uint8_t> make_stsd(uint32_t width, uint32_t height, const std::vector<uint8_t>& avcC) {
    std::vector<uint8_t> p;
    put_u32(p,1);
    append(p, make_video_sample_entry(width, height, box("avcC", avcC)));
    return full_box("stsd",0,0,p);
}

void put_ebml_id(std::vector<uint8_t>& b, uint32_t id) {
    if (id > 0x00ffffff) put_u32(b,id);
    else if (id > 0x0000ffff) { put_u8(b,uint8_t(id>>16)); put_u16(b,uint16_t(id)); }
    else if (id > 0x000000ff) put_u16(b,uint16_t(id));
    else put_u8(b,uint8_t(id));
}

void put_ebml_size(std::vector<uint8_t>& b, uint64_t size) {
    if (size < 0x7f) put_u8(b, uint8_t(0x80 | size));
    else if (size < 0x3fff) put_u16(b, uint16_t(0x4000 | size));
    else if (size < 0x1fffff) { put_u8(b, uint8_t(0x20 | (size >> 16))); put_u16(b, uint16_t(size)); }
    else { put_u8(b, uint8_t(0x10 | (size >> 24))); put_u24(b, uint32_t(size)); }
}

std::vector<uint8_t> ebml(uint32_t id, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> b;
    put_ebml_id(b,id);
    put_ebml_size(b,payload.size());
    append(b,payload);
    return b;
}

std::vector<uint8_t> ebml_uint(uint32_t id, uint64_t value) {
    std::vector<uint8_t> p;
    if (value <= 0xff) put_u8(p,uint8_t(value));
    else if (value <= 0xffff) put_u16(p,uint16_t(value));
    else put_u32(p,uint32_t(value));
    return ebml(id,p);
}

std::vector<uint8_t> ebml_string(uint32_t id, const std::string& value) {
    return ebml(id, std::vector<uint8_t>(value.begin(), value.end()));
}

std::vector<uint8_t> ebml_binary(uint32_t id, const std::vector<uint8_t>& value) {
    return ebml(id, value);
}

} // namespace

D3D12VideoEncodeBitstreamWriter::~D3D12VideoEncodeBitstreamWriter() {
    try { close(); } catch (...) {}
}

D3D12VideoEncodeBitstreamWriter::Container D3D12VideoEncodeBitstreamWriter::chooseContainer(const std::wstring& outputPath) const {
    const auto ext = lower_extension(std::filesystem::path(outputPath));
    if (ext == L".mp4" || ext == L".m4v") return Container::Mp4;
    if (ext == L".mkv") return Container::Mkv;
    if (ext == L".h264" || ext == L".264") return Container::Elementary;
    throw D3DVideoEncoderError("D3D12VideoEncodeBackend supports .h264/.264 elementary stream and .mp4/.mkv container output in the H.264 phase.");
}

bool D3D12VideoEncodeBitstreamWriter::isContainerMux() const noexcept {
    return container_ == Container::Mp4 || container_ == Container::Mkv;
}


void D3D12VideoEncodeBitstreamWriter::open(const std::wstring& outputPath) {
    const auto ext = lower_extension(std::filesystem::path(outputPath));
    if (ext == L".mp4" || ext == L".m4v" || ext == L".mkv") {
        throw D3DVideoEncoderError(
            "D3D12VideoEncodeBitstreamWriter container output requires width/height/fps/codec metadata. "
            "Call the metadata open() overload from D3D12VideoEncodeBackend.");
    }
    open(outputPath, 0, 0, 60, 1, VideoCodec::H264);
}

void D3D12VideoEncodeBitstreamWriter::open(
    const std::wstring& outputPath,
    uint32_t width,
    uint32_t height,
    uint32_t frameRateNum,
    uint32_t frameRateDen,
    VideoCodec codec) {

    if (outputPath.empty()) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBitstreamWriter output path is empty.");
    }
    if (codec != VideoCodec::H264) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBitstreamWriter currently supports only H.264 output.");
    }

    path_ = std::filesystem::path(outputPath);
    width_ = width;
    height_ = height;
    frameRateNum_ = frameRateNum;
    frameRateDen_ = frameRateDen;
    codec_ = codec;
    container_ = chooseContainer(outputPath);

    if (path_.has_parent_path()) {
        std::filesystem::create_directories(path_.parent_path());
    }

    if (container_ == Container::Elementary) {
        stream_.open(path_, std::ios::binary | std::ios::trunc);
        if (!stream_) {
            std::ostringstream oss;
            oss << "Failed to open D3D12 Video Encode elementary stream output: " << path_.string();
            throw D3DVideoEncoderError(oss.str());
        }
    }

    bytesWritten_ = 0;
    samples_.clear();
    avcSps_.clear();
    avcPps_.clear();
    opened_ = true;
}

void D3D12VideoEncodeBitstreamWriter::writeElementary(const uint8_t* data, size_t size) {
    stream_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!stream_) {
        throw D3DVideoEncoderError("Failed to write D3D12 Video Encode elementary stream output file.");
    }
    bytesWritten_ += static_cast<uint64_t>(size);
}

void D3D12VideoEncodeBitstreamWriter::parseParameterSetsAndSample(Sample& sample) {
    sample.lengthPrefixed = to_length_prefixed(sample.annexB);
    for (const auto& r : find_annexb_nalus(sample.annexB)) {
        const uint8_t* nal = sample.annexB.data() + r.first;
        if (r.second == 0) continue;
        const uint8_t type = nal[0] & 0x1f;
        if (type == 7) avcSps_.assign(nal, nal + r.second);
        else if (type == 8) avcPps_.assign(nal, nal + r.second);
        else if (type == 5) sample.keyFrame = true;
    }
}

void D3D12VideoEncodeBitstreamWriter::writeAccessUnit(
    const uint8_t* data,
    size_t size,
    int64_t timestamp100ns,
    int64_t duration100ns) {

    if (!opened_) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBitstreamWriter write called before open.");
    }
    if (!data && size > 0) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBitstreamWriter received null data.");
    }
    if (size == 0) {
        return;
    }

    if (container_ == Container::Elementary) {
        writeElementary(data, size);
        return;
    }

    Sample sample;
    sample.annexB.assign(data, data + size);
    sample.timestamp100ns = timestamp100ns;
    sample.duration100ns = duration100ns > 0
        ? duration100ns
        : static_cast<int64_t>(kTimeScale100ns) * frameRateDen_ / std::max<uint32_t>(frameRateNum_, 1);
    parseParameterSetsAndSample(sample);
    samples_.push_back(std::move(sample));
}

void D3D12VideoEncodeBitstreamWriter::flush() {
    if (stream_) {
        stream_.flush();
    }
}

void D3D12VideoEncodeBitstreamWriter::writeMp4() {
    if (samples_.empty()) {
        throw D3DVideoEncoderError("No D3D12 Video Encode samples were produced; cannot write MP4.");
    }

    const auto ftyp = make_ftyp();
    std::vector<uint8_t> mdatPayload;
    std::vector<uint64_t> offsets;
    offsets.reserve(samples_.size());
    const uint64_t mdatDataStart = ftyp.size() + 8;
    for (const auto& sample : samples_) {
        offsets.push_back(mdatDataStart + mdatPayload.size());
        append(mdatPayload, sample.lengthPrefixed);
    }
    std::vector<uint8_t> mdat = box("mdat", mdatPayload);

    uint64_t totalDuration = 0;
    for (const auto& sample : samples_) {
        totalDuration += static_cast<uint64_t>(std::max<int64_t>(1, sample.duration100ns));
    }

    struct Run { uint32_t count; uint32_t duration; };
    std::vector<Run> runs;
    for (const auto& sample : samples_) {
        const uint32_t duration = static_cast<uint32_t>(std::max<int64_t>(1, sample.duration100ns));
        if (!runs.empty() && runs.back().duration == duration) {
            ++runs.back().count;
        } else {
            runs.push_back({1, duration});
        }
    }

    std::vector<uint8_t> sttsP;
    put_u32(sttsP, static_cast<uint32_t>(runs.size()));
    for (const auto& run : runs) {
        put_u32(sttsP, run.count);
        put_u32(sttsP, run.duration);
    }
    auto stts = full_box("stts",0,0,sttsP);

    std::vector<uint8_t> stssP;
    uint32_t keyCount = 0;
    for (const auto& sample : samples_) if (sample.keyFrame) ++keyCount;
    put_u32(stssP, keyCount);
    for (size_t i = 0; i < samples_.size(); ++i) {
        if (samples_[i].keyFrame) put_u32(stssP, static_cast<uint32_t>(i + 1));
    }
    auto stss = full_box("stss",0,0,stssP);

    std::vector<uint8_t> stscP; put_u32(stscP,1); put_u32(stscP,1); put_u32(stscP,1); put_u32(stscP,1); auto stsc = full_box("stsc",0,0,stscP);
    std::vector<uint8_t> stszP; put_u32(stszP,0); put_u32(stszP,static_cast<uint32_t>(samples_.size())); for (const auto& s : samples_) put_u32(stszP,static_cast<uint32_t>(s.lengthPrefixed.size())); auto stsz = full_box("stsz",0,0,stszP);
    std::vector<uint8_t> stcoP; put_u32(stcoP,static_cast<uint32_t>(offsets.size()));
    for (auto offset : offsets) {
        if (offset > 0xffffffffull) throw D3DVideoEncoderError("MP4 file too large for stco; co64 is not implemented in this minimal muxer.");
        put_u32(stcoP, static_cast<uint32_t>(offset));
    }
    auto stco = full_box("stco",0,0,stcoP);

    const auto avcC = make_avcc(avcSps_, avcPps_);
    std::vector<uint8_t> stblP; append(stblP, make_stsd(width_, height_, avcC)); append(stblP, stts); if (keyCount > 0) append(stblP, stss); append(stblP, stsc); append(stblP, stsz); append(stblP, stco); auto stbl = box("stbl", stblP);
    std::vector<uint8_t> drefP; put_u32(drefP,1); append(drefP, full_box("url ",0,1,{})); auto dinf = box("dinf", full_box("dref",0,0,drefP));
    std::vector<uint8_t> vmhdP; put_u16(vmhdP,0); put_u16(vmhdP,0); put_u16(vmhdP,0); put_u16(vmhdP,0); auto vmhd = full_box("vmhd",0,1,vmhdP);
    std::vector<uint8_t> minfP; append(minfP, vmhd); append(minfP, dinf); append(minfP, stbl); auto minf = box("minf", minfP);
    std::vector<uint8_t> mdiaP; append(mdiaP, make_mdhd(totalDuration)); append(mdiaP, make_hdlr()); append(mdiaP, minf); auto mdia = box("mdia", mdiaP);
    std::vector<uint8_t> trakP; append(trakP, make_tkhd(width_, height_, totalDuration)); append(trakP, mdia); auto trak = box("trak", trakP);
    std::vector<uint8_t> moovP; append(moovP, make_mvhd(totalDuration)); append(moovP, trak); auto moov = box("moov", moovP);

    stream_.open(path_, std::ios::binary | std::ios::trunc);
    if (!stream_) throw D3DVideoEncoderError("Failed to open D3D12 Video Encode MP4 output file.");
    stream_.write(reinterpret_cast<const char*>(ftyp.data()), static_cast<std::streamsize>(ftyp.size()));
    stream_.write(reinterpret_cast<const char*>(mdat.data()), static_cast<std::streamsize>(mdat.size()));
    stream_.write(reinterpret_cast<const char*>(moov.data()), static_cast<std::streamsize>(moov.size()));
    if (!stream_) throw D3DVideoEncoderError("Failed to write D3D12 Video Encode MP4 output file.");
    bytesWritten_ = static_cast<uint64_t>(ftyp.size() + mdat.size() + moov.size());
}

void D3D12VideoEncodeBitstreamWriter::writeMkv() {
    if (samples_.empty()) {
        throw D3DVideoEncoderError("No D3D12 Video Encode samples were produced; cannot write MKV.");
    }
    const auto avcC = make_avcc(avcSps_, avcPps_);
    std::vector<uint8_t> ebmlHeader; append(ebmlHeader,ebml_uint(0x4286,1)); append(ebmlHeader,ebml_uint(0x42F7,1)); append(ebmlHeader,ebml_uint(0x42F2,4)); append(ebmlHeader,ebml_uint(0x42F3,8)); append(ebmlHeader,ebml_string(0x4282,"matroska")); append(ebmlHeader,ebml_uint(0x4287,4)); append(ebmlHeader,ebml_uint(0x4285,2)); ebmlHeader = ebml(0x1A45DFA3, ebmlHeader);
    std::vector<uint8_t> info; append(info,ebml_uint(0x2AD7B1,1000000)); append(info,ebml_string(0x4D80,"D3DVideoEncoder")); append(info,ebml_string(0x5741,"D3DVideoEncoder"));
    std::vector<uint8_t> video; append(video,ebml_uint(0xB0,width_)); append(video,ebml_uint(0xBA,height_));
    std::vector<uint8_t> track; append(track,ebml_uint(0xD7,1)); append(track,ebml_uint(0x73C5,1)); append(track,ebml_uint(0x83,1)); append(track,ebml_string(0x86,"V_MPEG4/ISO/AVC")); append(track,ebml_binary(0x63A2, avcC)); append(track,ebml(0xE0, video));
    std::vector<uint8_t> tracks = ebml(0x1654AE6B, ebml(0xAE, track));
    std::vector<uint8_t> clusterPayload; append(clusterPayload,ebml_uint(0xE7,0));
    for (const auto& sample : samples_) {
        std::vector<uint8_t> block;
        put_u8(block,0x81);
        put_i16(block, static_cast<int16_t>(sample.timestamp100ns / 10000));
        put_u8(block, sample.keyFrame ? 0x80 : 0x00);
        append(block, sample.lengthPrefixed);
        append(clusterPayload, ebml(0xA3, block));
    }
    std::vector<uint8_t> segment; append(segment, ebml(0x1549A966, info)); append(segment, tracks); append(segment, ebml(0x1F43B675, clusterPayload));
    auto seg = ebml(0x18538067, segment);

    stream_.open(path_, std::ios::binary | std::ios::trunc);
    if (!stream_) throw D3DVideoEncoderError("Failed to open D3D12 Video Encode MKV output file.");
    stream_.write(reinterpret_cast<const char*>(ebmlHeader.data()), static_cast<std::streamsize>(ebmlHeader.size()));
    stream_.write(reinterpret_cast<const char*>(seg.data()), static_cast<std::streamsize>(seg.size()));
    if (!stream_) throw D3DVideoEncoderError("Failed to write D3D12 Video Encode MKV output file.");
    bytesWritten_ = static_cast<uint64_t>(ebmlHeader.size() + seg.size());
}

void D3D12VideoEncodeBitstreamWriter::close() {
    if (!opened_) {
        return;
    }
    if (container_ == Container::Mp4) {
        writeMp4();
    } else if (container_ == Container::Mkv) {
        writeMkv();
    }
    if (stream_) {
        stream_.flush();
        stream_.close();
    }
    opened_ = false;
}

} // namespace D3DVideoEncoderLib
