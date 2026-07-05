#pragma once

#include "D3DVideoEncoderDesc.hpp"
#include "D3DVideoEncoderError.hpp"

#include <memory>

namespace D3DVideoEncoderLib {

class D3D11VideoEncoder {
public:
    explicit D3D11VideoEncoder(const D3D11VideoEncoderDesc& desc);
    ~D3D11VideoEncoder();

    D3D11VideoEncoder(const D3D11VideoEncoder&) = delete;
    D3D11VideoEncoder& operator=(const D3D11VideoEncoder&) = delete;

    D3D11VideoEncoder(D3D11VideoEncoder&&) noexcept;
    D3D11VideoEncoder& operator=(D3D11VideoEncoder&&) noexcept;

    void write(ID3D11Texture2D* texture);
    void write(ID3D11Texture2D* texture, int64_t timestamp100ns);

    void flush();
    void close();

    bool isOpen() const noexcept;
    uint64_t writtenFrameCount() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace D3DVideoEncoderLib
