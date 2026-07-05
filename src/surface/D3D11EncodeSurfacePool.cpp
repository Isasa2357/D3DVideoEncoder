#include "surface/D3D11EncodeSurfacePool.hpp"

#include <algorithm>
#include <stdexcept>

namespace D3DVideoEncoderLib {

void D3D11EncodeSurfacePool::initialize(
    D3D11CoreLib::D3D11Core& core,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    uint32_t count,
    bool blockingAcquire) {

    if (width == 0 || height == 0) {
        throw std::invalid_argument("D3D11EncodeSurfacePool requires non-zero width and height.");
    }
    if (format != DXGI_FORMAT_NV12 && format != DXGI_FORMAT_P010) {
        throw std::invalid_argument("D3D11EncodeSurfacePool supports only NV12/P010 encode surfaces.");
    }

    std::lock_guard<std::mutex> lock(mutex_);
    core_ = &core;
    width_ = width;
    height_ = height;
    format_ = format;
    blockingAcquire_ = blockingAcquire;
    nextIndex_ = 0;

    const uint32_t surfaceCount = std::max<uint32_t>(count, 3u);
    slots_.clear();
    slots_.reserve(surfaceCount);

    const UINT bindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    for (uint32_t i = 0; i < surfaceCount; ++i) {
        Slot slot;
        slot.resource = D3D11CoreLib::CreateTexture2D(
            core,
            width_,
            height_,
            format_,
            bindFlags,
            D3D11_USAGE_DEFAULT,
            0);
        slots_.push_back(slot);
    }
}

EncodeSurface D3D11EncodeSurfacePool::acquire() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!core_ || slots_.empty()) {
        throw std::runtime_error("D3D11EncodeSurfacePool is not initialized.");
    }

    auto findFree = [this]() -> std::size_t {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            const std::size_t index = (nextIndex_ + i) % slots_.size();
            if (!slots_[index].inUse) return index;
        }
        return static_cast<std::size_t>(-1);
    };

    std::size_t index = findFree();
    if (index == static_cast<std::size_t>(-1)) {
        if (!blockingAcquire_) {
            throw std::runtime_error("D3D11EncodeSurfacePool has no free surface.");
        }
        cv_.wait(lock, [&]() { return findFree() != static_cast<std::size_t>(-1); });
        index = findFree();
    }

    slots_[index].inUse = true;
    nextIndex_ = (index + 1) % slots_.size();

    D3D11CoreLib::D3D11Resource resource = slots_[index].resource;

    EncodeSurface surface;
    surface.api = EncodeSurface::Api::D3D11;
    surface.d3d11Resource = resource;
    surface.d3d11Texture = resource.AsTexture2D();
    surface.format = format_;
    surface.width = width_;
    surface.height = height_;
    surface.subresource = 0;
    surface.poolIndex = index;
    return surface;
}

void D3D11EncodeSurfacePool::release(const EncodeSurface& surface) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (surface.poolIndex < slots_.size()) {
        slots_[surface.poolIndex].inUse = false;
        cv_.notify_one();
    }
}

void D3D11EncodeSurfacePool::waitAllFree() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&]() {
        for (const auto& slot : slots_) {
            if (slot.inUse) return false;
        }
        return true;
    });
}

} // namespace D3DVideoEncoderLib
