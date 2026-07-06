#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>

namespace D3DVideoEncoderLib {

class D3D12VideoEncodeBitstreamWriter {
public:
    D3D12VideoEncodeBitstreamWriter() = default;
    ~D3D12VideoEncodeBitstreamWriter();

    D3D12VideoEncodeBitstreamWriter(const D3D12VideoEncodeBitstreamWriter&) = delete;
    D3D12VideoEncodeBitstreamWriter& operator=(const D3D12VideoEncodeBitstreamWriter&) = delete;

    void open(const std::wstring& path);
    void writeAccessUnit(const uint8_t* data, size_t size, int64_t timestamp100ns, int64_t duration100ns);
    void flush();
    void close();

    bool isOpen() const noexcept { return stream_.is_open(); }
    uint64_t bytesWritten() const noexcept { return bytesWritten_; }

private:
    std::filesystem::path path_;
    std::ofstream stream_;
    uint64_t bytesWritten_ = 0;
};

} // namespace D3DVideoEncoderLib
