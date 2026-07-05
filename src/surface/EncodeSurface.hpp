#pragma once

#include <cstddef>
#include <cstdint>
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <D3D11Helper/D3D11Framework/D3D11Resource.hpp>
#include <D3D12Helper/D3D12Framework/D3D12Resource.hpp>

namespace D3DVideoEncoderLib {

struct EncodeSurface {
    enum class Api {
        D3D11,
        D3D12,
    };

    Api api = Api::D3D11;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> d3d11Texture;
    Microsoft::WRL::ComPtr<ID3D12Resource> d3d12Resource;

    D3D11CoreLib::D3D11Resource d3d11Resource;
    D3D12CoreLib::D3D12Resource d3d12WrappedResource;

    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t subresource = 0;

    // Pool bookkeeping. Adapters own the pools; backend never mutates this.
    std::size_t poolIndex = static_cast<std::size_t>(-1);
    uint64_t poolGeneration = 0;
};

} // namespace D3DVideoEncoderLib
