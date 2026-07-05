#pragma once

#include "surface/EncodeSurface.hpp"

#include <condition_variable>
#include <mutex>
#include <vector>

#include <D3D11Helper/D3D11Core/D3D11Core.hpp>
#include <D3D11Helper/D3D11Framework/D3D11Framework.hpp>

namespace D3DVideoEncoderLib {

class D3D11EncodeSurfacePool {
public:
    D3D11EncodeSurfacePool() = default;

    void initialize(
        D3D11CoreLib::D3D11Core& core,
        uint32_t width,
        uint32_t height,
        DXGI_FORMAT format,
        uint32_t count,
        bool blockingAcquire = true);

    EncodeSurface acquire();
    void release(const EncodeSurface& surface);
    void waitAllFree();
    uint32_t surfaceCount() const;
    uint32_t activeCount() const;

private:
    struct Slot {
        D3D11CoreLib::D3D11Resource resource;
        bool inUse = false;
        uint64_t generation = 0;
    };

    D3D11CoreLib::D3D11Core* core_ = nullptr;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    bool blockingAcquire_ = true;
    uint64_t generation_ = 1;
    uint32_t activeCount_ = 0;

    std::vector<Slot> slots_;
    std::size_t nextIndex_ = 0;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
};

} // namespace D3DVideoEncoderLib
