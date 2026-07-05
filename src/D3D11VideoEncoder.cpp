#include <D3DVideoEncoder/D3D11VideoEncoder.hpp>

#include "D3D11VideoEncoderImpl.hpp"

namespace D3DVideoEncoderLib {

D3D11VideoEncoder::D3D11VideoEncoder(const D3D11VideoEncoderDesc& desc)
    : impl_(std::make_unique<Impl>(desc)) {}

D3D11VideoEncoder::~D3D11VideoEncoder() = default;
D3D11VideoEncoder::D3D11VideoEncoder(D3D11VideoEncoder&&) noexcept = default;
D3D11VideoEncoder& D3D11VideoEncoder::operator=(D3D11VideoEncoder&&) noexcept = default;

void D3D11VideoEncoder::write(ID3D11Texture2D* texture) {
    impl_->write(texture);
}

void D3D11VideoEncoder::write(ID3D11Texture2D* texture, int64_t timestamp100ns) {
    impl_->write(texture, timestamp100ns);
}

void D3D11VideoEncoder::flush() {
    impl_->flush();
}

void D3D11VideoEncoder::close() {
    impl_->close();
}

bool D3D11VideoEncoder::isOpen() const noexcept {
    return impl_ && impl_->isOpen();
}

uint64_t D3D11VideoEncoder::writtenFrameCount() const noexcept {
    return impl_ ? impl_->writtenFrameCount() : 0;
}

} // namespace D3DVideoEncoderLib
