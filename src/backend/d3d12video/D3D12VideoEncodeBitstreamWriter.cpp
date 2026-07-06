#include "backend/d3d12video/D3D12VideoEncodeBitstreamWriter.hpp"

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

#include <algorithm>
#include <cwctype>
#include <sstream>

namespace D3DVideoEncoderLib {
namespace {

std::wstring lower_extension(std::filesystem::path path) {
    std::wstring ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return ext;
}

} // namespace

D3D12VideoEncodeBitstreamWriter::~D3D12VideoEncodeBitstreamWriter() {
    try { close(); } catch (...) {}
}

void D3D12VideoEncodeBitstreamWriter::open(const std::wstring& path) {
    if (path.empty()) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBitstreamWriter output path is empty.");
    }
    path_ = std::filesystem::path(path);
    const std::wstring ext = lower_extension(path_);
    if (ext != L".h264" && ext != L".264") {
        throw D3DVideoEncoderError(
            "D3D12VideoEncodeBackend Phase 2 writes only H.264 elementary streams. "
            "Use an output path ending in .h264 or .264. MP4/MKV mux is a later phase.");
    }

    if (path_.has_parent_path()) {
        std::filesystem::create_directories(path_.parent_path());
    }

    stream_.open(path_, std::ios::binary | std::ios::trunc);
    if (!stream_) {
        std::ostringstream oss;
        oss << "Failed to open D3D12 Video Encode bitstream output: " << path_.string();
        throw D3DVideoEncoderError(oss.str());
    }
    bytesWritten_ = 0;
}

void D3D12VideoEncodeBitstreamWriter::writeAccessUnit(
    const uint8_t* data,
    size_t size,
    int64_t timestamp100ns,
    int64_t duration100ns) {

    (void)timestamp100ns;
    (void)duration100ns;
    if (!stream_) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBitstreamWriter is not open.");
    }
    if (!data && size > 0) {
        throw D3DVideoEncoderError("D3D12VideoEncodeBitstreamWriter received null data.");
    }
    if (size == 0) {
        return;
    }

    stream_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    if (!stream_) {
        throw D3DVideoEncoderError("Failed while writing D3D12 Video Encode bitstream.");
    }
    bytesWritten_ += static_cast<uint64_t>(size);
}

void D3D12VideoEncodeBitstreamWriter::flush() {
    if (stream_) {
        stream_.flush();
    }
}

void D3D12VideoEncodeBitstreamWriter::close() {
    if (stream_) {
        stream_.flush();
        stream_.close();
    }
}

} // namespace D3DVideoEncoderLib
