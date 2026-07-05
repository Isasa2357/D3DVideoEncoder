#include "backend/mux/NvencOutputMuxer.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <sstream>

namespace D3DVideoEncoderLib {
namespace {

constexpr uint32_t kTimeScale100ns = 10'000'000;

std::wstring LowerExtension(const std::wstring& path) {
    std::wstring ext = std::filesystem::path(path).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return ext;
}

void PutU8(std::vector<uint8_t>& b, uint8_t v) { b.push_back(v); }
void PutU16(std::vector<uint8_t>& b, uint16_t v) { b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
void PutU24(std::vector<uint8_t>& b, uint32_t v) { b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
void PutU32(std::vector<uint8_t>& b, uint32_t v) { b.push_back(uint8_t(v >> 24)); b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v)); }
void PutU64(std::vector<uint8_t>& b, uint64_t v) { PutU32(b, uint32_t(v >> 32)); PutU32(b, uint32_t(v)); }
void PutI16(std::vector<uint8_t>& b, int16_t v) { PutU16(b, static_cast<uint16_t>(v)); }

void PutType(std::vector<uint8_t>& b, const char (&type)[5]) { b.insert(b.end(), type, type + 4); }

std::vector<uint8_t> Box(const char (&type)[5], const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> b;
    const uint64_t size = 8ull + payload.size();
    if (size > 0xffffffffull) throw D3DVideoEncoderError("MP4 box is too large for this minimal muxer.");
    PutU32(b, static_cast<uint32_t>(size));
    PutType(b, type);
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

void Append(std::vector<uint8_t>& dst, const std::vector<uint8_t>& src) { dst.insert(dst.end(), src.begin(), src.end()); }

std::vector<uint8_t> FullBox(const char (&type)[5], uint8_t version, uint32_t flags, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> p;
    PutU8(p, version);
    PutU24(p, flags);
    p.insert(p.end(), payload.begin(), payload.end());
    return Box(type, p);
}

bool IsStartCode3(const uint8_t* p) noexcept { return p[0] == 0 && p[1] == 0 && p[2] == 1; }
bool IsStartCode4(const uint8_t* p) noexcept { return p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1; }

std::vector<std::pair<size_t, size_t>> FindAnnexBNalus(const std::vector<uint8_t>& bytes) {
    std::vector<std::pair<size_t, size_t>> ranges;
    size_t i = 0;
    while (i + 3 <= bytes.size()) {
        size_t sc = std::string::npos;
        size_t scLen = 0;
        for (; i + 3 <= bytes.size(); ++i) {
            if (i + 4 <= bytes.size() && IsStartCode4(bytes.data() + i)) { sc = i; scLen = 4; break; }
            if (IsStartCode3(bytes.data() + i)) { sc = i; scLen = 3; break; }
        }
        if (sc == std::string::npos) break;
        size_t nalStart = sc + scLen;
        i = nalStart;
        size_t next = bytes.size();
        for (; i + 3 <= bytes.size(); ++i) {
            if ((i + 4 <= bytes.size() && IsStartCode4(bytes.data() + i)) || IsStartCode3(bytes.data() + i)) { next = i; break; }
        }
        while (nalStart < next && bytes[nalStart] == 0) ++nalStart;
        if (next > nalStart) ranges.emplace_back(nalStart, next - nalStart);
    }
    if (ranges.empty() && !bytes.empty()) ranges.emplace_back(0, bytes.size());
    return ranges;
}

std::vector<uint8_t> ToLengthPrefixed(const std::vector<uint8_t>& annexB) {
    std::vector<uint8_t> out;
    for (const auto& r : FindAnnexBNalus(annexB)) {
        PutU32(out, static_cast<uint32_t>(r.second));
        out.insert(out.end(), annexB.begin() + static_cast<std::ptrdiff_t>(r.first), annexB.begin() + static_cast<std::ptrdiff_t>(r.first + r.second));
    }
    return out;
}

bool IsH264KeyNal(uint8_t nalHeader) noexcept { return (nalHeader & 0x1f) == 5; }
bool IsH265KeyNal(uint8_t nalHeader) noexcept { const uint8_t t = (nalHeader >> 1) & 0x3f; return t >= 16 && t <= 21; }

std::vector<uint8_t> MakeAvcC(const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps) {
    if (sps.size() < 4 || pps.empty()) throw D3DVideoEncoderError("MP4/H.264 mux requires SPS and PPS in the NVENC bitstream.");
    std::vector<uint8_t> p;
    PutU8(p, 1);
    PutU8(p, sps[1]);
    PutU8(p, sps[2]);
    PutU8(p, sps[3]);
    PutU8(p, 0xff); // lengthSizeMinusOne = 3
    PutU8(p, 0xe1); // one SPS
    PutU16(p, static_cast<uint16_t>(sps.size())); p.insert(p.end(), sps.begin(), sps.end());
    PutU8(p, 1); // one PPS
    PutU16(p, static_cast<uint16_t>(pps.size())); p.insert(p.end(), pps.begin(), pps.end());
    return p;
}

std::vector<uint8_t> MakeHvcC(const std::vector<uint8_t>& vps, const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps) {
    if (vps.empty() || sps.empty() || pps.empty()) throw D3DVideoEncoderError("MP4/HEVC mux requires VPS, SPS and PPS in the NVENC bitstream.");
    std::vector<uint8_t> p;
    PutU8(p, 1);       // configurationVersion
    PutU8(p, 1);       // profile/tier/space: conservative default
    PutU32(p, 0);      // profile compatibility
    PutU32(p, 0); PutU16(p, 0); // constraint flags
    PutU8(p, 120);     // level_idc fallback
    PutU16(p, 0xf000); // min_spatial_segmentation_idc
    PutU8(p, 0xfc);    // parallelismType
    PutU8(p, 0xfc);    // chromaFormat
    PutU8(p, 0xf8);    // bitDepthLumaMinus8
    PutU8(p, 0xf8);    // bitDepthChromaMinus8
    PutU16(p, 0);      // avgFrameRate
    PutU8(p, 0x0f);    // constantFrameRate/numTemporalLayers/temporalIdNested/lengthSizeMinusOne
    PutU8(p, 3);       // numOfArrays
    auto addArray = [&](uint8_t nalType, const std::vector<uint8_t>& nal) {
        PutU8(p, 0x80 | nalType);
        PutU16(p, 1);
        PutU16(p, static_cast<uint16_t>(nal.size()));
        p.insert(p.end(), nal.begin(), nal.end());
    };
    addArray(32, vps); addArray(33, sps); addArray(34, pps);
    return p;
}

std::vector<uint8_t> MakeFtyp(VideoCodec codec) {
    std::vector<uint8_t> p;
    if (codec == VideoCodec::HEVC) PutType(p, "isom"); else PutType(p, "isom");
    PutU32(p, 0x200);
    PutType(p, "isom"); PutType(p, "iso2"); PutType(p, "mp41");
    if (codec == VideoCodec::HEVC) PutType(p, "hvc1"); else PutType(p, "avc1");
    return Box("ftyp", p);
}

std::vector<uint8_t> MakeMvhd(uint64_t duration) {
    std::vector<uint8_t> p;
    PutU32(p, 0); PutU32(p, 0); PutU32(p, kTimeScale100ns); PutU32(p, static_cast<uint32_t>(duration));
    PutU32(p, 0x00010000); PutU16(p, 0x0100); PutU16(p, 0); PutU32(p, 0); PutU32(p, 0);
    const uint32_t matrix[9] = {0x00010000,0,0,0,0x00010000,0,0,0,0x40000000};
    for (uint32_t v: matrix) PutU32(p, v);
    for (int i=0;i<6;++i) PutU32(p,0);
    PutU32(p, 2);
    return FullBox("mvhd", 0, 0, p);
}

std::vector<uint8_t> MakeTkhd(uint32_t width, uint32_t height, uint64_t duration) {
    std::vector<uint8_t> p;
    PutU32(p,0); PutU32(p,0); PutU32(p,1); PutU32(p,0); PutU32(p,static_cast<uint32_t>(duration));
    PutU32(p,0); PutU32(p,0); PutU16(p,0); PutU16(p,0); PutU16(p,0); PutU16(p,0);
    const uint32_t matrix[9] = {0x00010000,0,0,0,0x00010000,0,0,0,0x40000000};
    for (uint32_t v: matrix) PutU32(p, v);
    PutU32(p, width << 16); PutU32(p, height << 16);
    return FullBox("tkhd", 0, 0x000007, p);
}

std::vector<uint8_t> MakeMdhd(uint64_t duration) {
    std::vector<uint8_t> p;
    PutU32(p,0); PutU32(p,0); PutU32(p,kTimeScale100ns); PutU32(p,static_cast<uint32_t>(duration));
    PutU16(p,0x55c4); PutU16(p,0);
    return FullBox("mdhd",0,0,p);
}

std::vector<uint8_t> MakeHdlr() {
    std::vector<uint8_t> p;
    PutU32(p,0); PutType(p,"vide"); PutU32(p,0); PutU32(p,0); PutU32(p,0);
    const char name[] = "VideoHandler";
    p.insert(p.end(), name, name + sizeof(name));
    return FullBox("hdlr",0,0,p);
}

std::vector<uint8_t> MakeVideoSampleEntry(const char (&coding)[5], uint32_t width, uint32_t height, const std::vector<uint8_t>& configBox) {
    std::vector<uint8_t> p;
    p.resize(6,0); PutU16(p,1); // reserved + data_reference_index
    PutU16(p,0); PutU16(p,0); PutU32(p,0); PutU32(p,0); PutU32(p,0);
    PutU16(p,static_cast<uint16_t>(width)); PutU16(p,static_cast<uint16_t>(height));
    PutU32(p,0x00480000); PutU32(p,0x00480000); PutU32(p,0); PutU16(p,1);
    p.push_back(0); p.resize(p.size()+31,0); PutU16(p,0x18); PutU16(p,0xffff);
    Append(p, configBox);
    return Box(coding, p);
}

std::vector<uint8_t> MakeStsd(VideoCodec codec, uint32_t width, uint32_t height, const std::vector<uint8_t>& codecConfig) {
    std::vector<uint8_t> p; PutU32(p,1);
    if (codec == VideoCodec::HEVC) Append(p, MakeVideoSampleEntry("hvc1", width, height, Box("hvcC", codecConfig)));
    else Append(p, MakeVideoSampleEntry("avc1", width, height, Box("avcC", codecConfig)));
    return FullBox("stsd",0,0,p);
}

// EBML helpers
void PutEbmlId(std::vector<uint8_t>& b, uint32_t id) {
    if (id > 0x00ffffff) PutU32(b,id);
    else if (id > 0x0000ffff) { PutU8(b,uint8_t(id>>16)); PutU16(b,uint16_t(id)); }
    else if (id > 0x000000ff) PutU16(b,uint16_t(id));
    else PutU8(b,uint8_t(id));
}

void PutEbmlSize(std::vector<uint8_t>& b, uint64_t size) {
    if (size < 0x7f) PutU8(b, uint8_t(0x80 | size));
    else if (size < 0x3fff) PutU16(b, uint16_t(0x4000 | size));
    else if (size < 0x1fffff) { PutU8(b, uint8_t(0x20 | (size >> 16))); PutU16(b, uint16_t(size)); }
    else { PutU8(b, uint8_t(0x10 | (size >> 24))); PutU24(b, uint32_t(size)); }
}
std::vector<uint8_t> Ebml(uint32_t id, const std::vector<uint8_t>& payload) { std::vector<uint8_t> b; PutEbmlId(b,id); PutEbmlSize(b,payload.size()); Append(b,payload); return b; }
std::vector<uint8_t> EbmlUInt(uint32_t id, uint64_t value) { std::vector<uint8_t> p; if (value <= 0xff) PutU8(p,uint8_t(value)); else if (value <= 0xffff) PutU16(p,uint16_t(value)); else PutU32(p,uint32_t(value)); return Ebml(id,p); }
std::vector<uint8_t> EbmlString(uint32_t id, const std::string& value) { return Ebml(id, std::vector<uint8_t>(value.begin(), value.end())); }
std::vector<uint8_t> EbmlBinary(uint32_t id, const std::vector<uint8_t>& value) { return Ebml(id, value); }

} // namespace

NvencOutputMuxer::~NvencOutputMuxer() { try { close(); } catch (...) {} }

NvencOutputMuxer::Container NvencOutputMuxer::chooseContainer(const std::wstring& outputPath) const {
    const auto ext = LowerExtension(outputPath);
    if (ext == L".mp4" || ext == L".m4v") return Container::Mp4;
    if (ext == L".mkv") return Container::Mkv;
    return Container::Elementary;
}

bool NvencOutputMuxer::isContainerMux() const noexcept { return container_ == Container::Mp4 || container_ == Container::Mkv; }

void NvencOutputMuxer::open(const std::wstring& outputPath, uint32_t width, uint32_t height, uint32_t frameRateNum, uint32_t frameRateDen, VideoCodec codec) {
    outputPath_ = outputPath; width_ = width; height_ = height; frameRateNum_ = frameRateNum; frameRateDen_ = frameRateDen; codec_ = codec;
    if (outputPath_.empty()) throw D3DVideoEncoderError("NVENC muxer outputPath is empty.");
    container_ = chooseContainer(outputPath_);
    if (codec_ == VideoCodec::AV1 && isContainerMux()) throw D3DVideoEncoderError("NVENC MP4/MKV muxer currently supports H.264/HEVC. AV1 mux is not implemented.");
    if (container_ == Container::Elementary) {
        output_.open(std::filesystem::path(outputPath_), std::ios::binary | std::ios::trunc);
        if (!output_) throw D3DVideoEncoderError("Failed to open NVENC elementary stream output file.");
    }
    opened_ = true;
}

void NvencOutputMuxer::writeElementary(const uint8_t* data, size_t size) {
    output_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!output_) throw D3DVideoEncoderError("Failed to write NVENC elementary stream output file.");
}

void NvencOutputMuxer::parseParameterSetsAndSample(Sample& sample) {
    sample.lengthPrefixed = ToLengthPrefixed(sample.annexB);
    for (const auto& r : FindAnnexBNalus(sample.annexB)) {
        const uint8_t* nal = sample.annexB.data() + r.first;
        if (r.second == 0) continue;
        if (codec_ == VideoCodec::H264) {
            const uint8_t type = nal[0] & 0x1f;
            if (type == 7) avcSps_.assign(nal, nal + r.second);
            else if (type == 8) avcPps_.assign(nal, nal + r.second);
            else if (type == 5) sample.keyFrame = true;
        } else if (codec_ == VideoCodec::HEVC) {
            if (r.second < 2) continue;
            const uint8_t type = (nal[0] >> 1) & 0x3f;
            if (type == 32) hevcVps_.assign(nal, nal + r.second);
            else if (type == 33) hevcSps_.assign(nal, nal + r.second);
            else if (type == 34) hevcPps_.assign(nal, nal + r.second);
            else if (type >= 16 && type <= 21) sample.keyFrame = true;
        }
    }
}

void NvencOutputMuxer::writeAccessUnit(const uint8_t* data, size_t size, int64_t timestamp100ns, int64_t duration100ns) {
    if (!opened_) throw D3DVideoEncoderError("NVENC muxer write called before open.");
    if (!data || size == 0) return;
    if (container_ == Container::Elementary) { writeElementary(data, size); return; }
    Sample s;
    s.annexB.assign(data, data + size);
    s.timestamp100ns = timestamp100ns;
    s.duration100ns = duration100ns > 0 ? duration100ns : static_cast<int64_t>(kTimeScale100ns) * frameRateDen_ / frameRateNum_;
    parseParameterSetsAndSample(s);
    samples_.push_back(std::move(s));
}

void NvencOutputMuxer::flush() { if (output_) output_.flush(); }

void NvencOutputMuxer::writeMp4() {
    if (samples_.empty()) throw D3DVideoEncoderError("No NVENC samples were produced; cannot write MP4.");
    const auto ftyp = MakeFtyp(codec_);
    std::vector<uint8_t> mdatPayload;
    std::vector<uint64_t> offsets;
    offsets.reserve(samples_.size());
    const uint64_t mdatDataStart = ftyp.size() + 8;
    for (const auto& s : samples_) {
        offsets.push_back(mdatDataStart + mdatPayload.size());
        Append(mdatPayload, s.lengthPrefixed);
    }
    std::vector<uint8_t> mdat = Box("mdat", mdatPayload);

    uint64_t totalDuration = 0; for (const auto& s : samples_) totalDuration += static_cast<uint64_t>(std::max<int64_t>(1, s.duration100ns));
    std::vector<uint8_t> sttsP; // entry_count + runs
    struct Run { uint32_t count; uint32_t dur; };
    std::vector<Run> runs;
    for (const auto& s : samples_) {
        uint32_t d = static_cast<uint32_t>(std::max<int64_t>(1, s.duration100ns));
        if (!runs.empty() && runs.back().dur == d) runs.back().count++; else runs.push_back({1,d});
    }
    PutU32(sttsP, static_cast<uint32_t>(runs.size())); for (auto r: runs) { PutU32(sttsP,r.count); PutU32(sttsP,r.dur); }
    auto stts = FullBox("stts",0,0,sttsP);

    std::vector<uint8_t> stssP; uint32_t keyCount=0; for (auto& s:samples_) if (s.keyFrame) keyCount++; PutU32(stssP,keyCount); for (size_t i=0;i<samples_.size();++i) if (samples_[i].keyFrame) PutU32(stssP,uint32_t(i+1));
    auto stss = FullBox("stss",0,0,stssP);

    std::vector<uint8_t> stscP; PutU32(stscP,1); PutU32(stscP,1); PutU32(stscP,1); PutU32(stscP,1); auto stsc = FullBox("stsc",0,0,stscP);
    std::vector<uint8_t> stszP; PutU32(stszP,0); PutU32(stszP,uint32_t(samples_.size())); for (auto& s:samples_) PutU32(stszP,uint32_t(s.lengthPrefixed.size())); auto stsz=FullBox("stsz",0,0,stszP);
    std::vector<uint8_t> stcoP; PutU32(stcoP,uint32_t(offsets.size())); for (auto off:offsets) { if(off>0xffffffffull) throw D3DVideoEncoderError("MP4 file too large for stco; co64 is not implemented in this minimal muxer."); PutU32(stcoP,uint32_t(off)); } auto stco=FullBox("stco",0,0,stcoP);

    std::vector<uint8_t> codecConfig = codec_ == VideoCodec::HEVC ? MakeHvcC(hevcVps_, hevcSps_, hevcPps_) : MakeAvcC(avcSps_, avcPps_);
    std::vector<uint8_t> stblP; Append(stblP, MakeStsd(codec_, width_, height_, codecConfig)); Append(stblP, stts); if (keyCount>0) Append(stblP, stss); Append(stblP, stsc); Append(stblP, stsz); Append(stblP, stco); auto stbl = Box("stbl", stblP);
    std::vector<uint8_t> drefP; PutU32(drefP,1); Append(drefP, FullBox("url ",0,1,{})); auto dinf = Box("dinf", FullBox("dref",0,0,drefP));
    std::vector<uint8_t> vmhdP; PutU16(vmhdP,0); PutU16(vmhdP,0); PutU16(vmhdP,0); PutU16(vmhdP,0); auto vmhd = FullBox("vmhd",0,1,vmhdP);
    std::vector<uint8_t> minfP; Append(minfP,vmhd); Append(minfP,dinf); Append(minfP,stbl); auto minf=Box("minf",minfP);
    std::vector<uint8_t> mdiaP; Append(mdiaP,MakeMdhd(totalDuration)); Append(mdiaP,MakeHdlr()); Append(mdiaP,minf); auto mdia=Box("mdia",mdiaP);
    std::vector<uint8_t> trakP; Append(trakP,MakeTkhd(width_,height_,totalDuration)); Append(trakP,mdia); auto trak=Box("trak",trakP);
    std::vector<uint8_t> moovP; Append(moovP,MakeMvhd(totalDuration)); Append(moovP,trak); auto moov=Box("moov",moovP);

    output_.open(std::filesystem::path(outputPath_), std::ios::binary | std::ios::trunc);
    if (!output_) throw D3DVideoEncoderError("Failed to open NVENC MP4 output file.");
    output_.write(reinterpret_cast<const char*>(ftyp.data()), static_cast<std::streamsize>(ftyp.size()));
    output_.write(reinterpret_cast<const char*>(mdat.data()), static_cast<std::streamsize>(mdat.size()));
    output_.write(reinterpret_cast<const char*>(moov.data()), static_cast<std::streamsize>(moov.size()));
    if (!output_) throw D3DVideoEncoderError("Failed to write NVENC MP4 output file.");
}

void NvencOutputMuxer::writeMkv() {
    if (samples_.empty()) throw D3DVideoEncoderError("No NVENC samples were produced; cannot write MKV.");
    std::vector<uint8_t> codecPrivate = codec_ == VideoCodec::HEVC ? MakeHvcC(hevcVps_, hevcSps_, hevcPps_) : MakeAvcC(avcSps_, avcPps_);
    std::vector<uint8_t> ebmlHeader; Append(ebmlHeader,EbmlUInt(0x4286,1)); Append(ebmlHeader,EbmlUInt(0x42F7,1)); Append(ebmlHeader,EbmlUInt(0x42F2,4)); Append(ebmlHeader,EbmlUInt(0x42F3,8)); Append(ebmlHeader,EbmlString(0x4282,"matroska")); Append(ebmlHeader,EbmlUInt(0x4287,4)); Append(ebmlHeader,EbmlUInt(0x4285,2)); ebmlHeader = Ebml(0x1A45DFA3, ebmlHeader);
    std::vector<uint8_t> info; Append(info,EbmlUInt(0x2AD7B1,1000000)); Append(info,EbmlString(0x4D80,"D3DVideoEncoder")); Append(info,EbmlString(0x5741,"D3DVideoEncoder"));
    std::vector<uint8_t> video; Append(video,EbmlUInt(0xB0,width_)); Append(video,EbmlUInt(0xBA,height_));
    std::vector<uint8_t> track; Append(track,EbmlUInt(0xD7,1)); Append(track,EbmlUInt(0x73C5,1)); Append(track,EbmlUInt(0x83,1)); Append(track,EbmlString(0x86, codec_==VideoCodec::HEVC?"V_MPEGH/ISO/HEVC":"V_MPEG4/ISO/AVC")); Append(track,EbmlBinary(0x63A2, codecPrivate)); Append(track,Ebml(0xE0, video));
    std::vector<uint8_t> tracks = Ebml(0x1654AE6B, Ebml(0xAE, track));
    std::vector<uint8_t> clusterPayload; Append(clusterPayload,EbmlUInt(0xE7,0));
    for (const auto& s : samples_) {
        std::vector<uint8_t> block; PutU8(block,0x81); PutI16(block, static_cast<int16_t>(s.timestamp100ns / 10000)); PutU8(block, s.keyFrame?0x80:0x00); Append(block,s.lengthPrefixed); Append(clusterPayload,Ebml(0xA3,block));
    }
    std::vector<uint8_t> segment; Append(segment,Ebml(0x1549A966,info)); Append(segment,tracks); Append(segment,Ebml(0x1F43B675,clusterPayload));
    auto seg = Ebml(0x18538067, segment);
    output_.open(std::filesystem::path(outputPath_), std::ios::binary | std::ios::trunc);
    if (!output_) throw D3DVideoEncoderError("Failed to open NVENC MKV output file.");
    output_.write(reinterpret_cast<const char*>(ebmlHeader.data()), static_cast<std::streamsize>(ebmlHeader.size()));
    output_.write(reinterpret_cast<const char*>(seg.data()), static_cast<std::streamsize>(seg.size()));
    if (!output_) throw D3DVideoEncoderError("Failed to write NVENC MKV output file.");
}

void NvencOutputMuxer::close() {
    if (!opened_) return;
    if (container_ == Container::Mp4) writeMp4();
    else if (container_ == Container::Mkv) writeMkv();
    if (output_) output_.close();
    opened_ = false;
}

} // namespace D3DVideoEncoderLib
