#include "surface/D3D11EncodeSurfacePool.hpp"

#include <algorithm>
#include <limits>
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

    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this]() { return activeCount_ == 0; });

    core_ = &core;
    width_ = width;
    height_ = height;
    format_ = format;
    blockingAcquire_ = blockingAcquire;
    nextIndex_ = 0;
    activeCount_ = 0;
    if (generation_ == std::numeric_limits<uint64_t>::max()) {
        generation_ = 1;
    } else {
        ++generation_;
    }

    const uint32_t surfaceCount = std::max<uint32_t>(count, 3u);
    slots_.clear();
    slots_.reserve(surfaceCount);

    const UINT bindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    for (uint32_t i = 0; i < surfaceCount; ++i) {
        Slot slot;
        slot.generation = generation_;
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
    ++activeCount_;
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
    surface.poolGeneration = generation_;
    return surface;
}

void D3D11EncodeSurfacePool::release(const EncodeSurface& surface) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (surface.poolIndex >= slots_.size()) {
        return;
    }

    Slot& slot = slots_[surface.poolIndex];
    if (slot.generation != surface.poolGeneration || !slot.inUse) {
        return;
    }

    slot.inUse = false;
    if (activeCount_ > 0) {
        --activeCount_;
    }
    cv_.notify_all();
}

void D3D11EncodeSurfacePool::waitAllFree() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&]() { return activeCount_ == 0; });
}

uint32_t D3D11EncodeSurfacePool::surfaceCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint32_t>(slots_.size());
}

uint32_t D3D11EncodeSurfacePool::activeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return activeCount_;
}

} // namespace D3DVideoEncoderLib
