#include <D3DVideoEncoder/D3DVideoEncoder.hpp>

namespace D3DVideoEncoderLib {

D3DVideoEncoder::D3DVideoEncoder(const D3DVideoEncoderDesc& desc)
    : api_(desc.inputApi) {
    switch (api_) {
    case D3DVideoInputApi::D3D11:
        d3d11_ = std::make_unique<D3D11VideoEncoder>(ToD3D11Desc(desc));
        break;
    case D3DVideoInputApi::D3D12:
        d3d12_ = std::make_unique<D3D12VideoEncoder>(ToD3D12Desc(desc));
        break;
    default:
        throw D3DVideoEncoderError("Unsupported D3DVideoEncoderDesc.inputApi.");
    }
}

D3DVideoEncoder::~D3DVideoEncoder() = default;
D3DVideoEncoder::D3DVideoEncoder(D3DVideoEncoder&&) noexcept = default;
D3DVideoEncoder& D3DVideoEncoder::operator=(D3DVideoEncoder&&) noexcept = default;

void D3DVideoEncoder::write(ID3D11Texture2D* texture) {
    if (!d3d11_) throw D3DVideoEncoderError("write(ID3D11Texture2D*) called on non-D3D11 encoder.");
    d3d11_->write(texture);
}

void D3DVideoEncoder::write(ID3D11Texture2D* texture, int64_t timestamp100ns) {
    if (!d3d11_) throw D3DVideoEncoderError("write(ID3D11Texture2D*) called on non-D3D11 encoder.");
    d3d11_->write(texture, timestamp100ns);
}

void D3DVideoEncoder::write(ID3D11Texture2D* texture, int64_t timestamp100ns, int64_t duration100ns) {
    if (!d3d11_) throw D3DVideoEncoderError("write(ID3D11Texture2D*) called on non-D3D11 encoder.");
    d3d11_->write(texture, timestamp100ns, duration100ns);
}

void D3DVideoEncoder::write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState) {
    if (!d3d12_) throw D3DVideoEncoderError("write(ID3D12Resource*) called on non-D3D12 encoder.");
    d3d12_->write(resource, currentState);
}

void D3DVideoEncoder::write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns) {
    if (!d3d12_) throw D3DVideoEncoderError("write(ID3D12Resource*) called on non-D3D12 encoder.");
    d3d12_->write(resource, currentState, timestamp100ns);
}

void D3DVideoEncoder::write(ID3D12Resource* resource, D3D12_RESOURCE_STATES currentState, int64_t timestamp100ns, int64_t duration100ns) {
    if (!d3d12_) throw D3DVideoEncoderError("write(ID3D12Resource*) called on non-D3D12 encoder.");
    d3d12_->write(resource, currentState, timestamp100ns, duration100ns);
}

void D3DVideoEncoder::flush() {
    if (d3d11_) d3d11_->flush();
    if (d3d12_) d3d12_->flush();
}

void D3DVideoEncoder::close() {
    if (d3d11_) d3d11_->close();
    if (d3d12_) d3d12_->close();
}

bool D3DVideoEncoder::isOpen() const noexcept {
    return (d3d11_ && d3d11_->isOpen()) || (d3d12_ && d3d12_->isOpen());
}

uint64_t D3DVideoEncoder::writtenFrameCount() const noexcept {
    if (d3d11_) return d3d11_->writtenFrameCount();
    if (d3d12_) return d3d12_->writtenFrameCount();
    return 0;
}

} // namespace D3DVideoEncoderLib
