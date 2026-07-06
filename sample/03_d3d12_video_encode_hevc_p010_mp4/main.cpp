#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace D3DVideoEncoderLib;

namespace {

void throw_if_failed(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        std::ostringstream oss;
        oss << message << " failed. HRESULT=0x" << std::hex << static_cast<unsigned long>(hr);
        throw std::runtime_error(oss.str());
    }
}

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) noexcept {
    D3D12_HEAP_PROPERTIES props = {};
    props.Type = type;
    props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    props.CreationNodeMask = 1;
    props.VisibleNodeMask = 1;
    return props;
}

D3D12_RESOURCE_DESC buffer_desc(uint64_t size) noexcept {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return desc;
}

D3D12_RESOURCE_DESC p010_texture_desc(uint32_t width, uint32_t height) noexcept {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_P010;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return desc;
}

void wait_for_queue(ID3D12Device* device, ID3D12CommandQueue* queue) {
    ComPtr<ID3D12Fence> fence;
    throw_if_failed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    throw_if_failed(queue->Signal(fence.Get(), 1), "Signal");
    if (fence->GetCompletedValue() < 1) {
        HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) throw std::runtime_error("CreateEventW failed");
        throw_if_failed(fence->SetEventOnCompletion(1, eventHandle), "SetEventOnCompletion");
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }
}

void write_u16(uint8_t* dst, uint16_t value) {
    dst[0] = static_cast<uint8_t>(value & 0xffu);
    dst[1] = static_cast<uint8_t>(value >> 8);
}

void upload_p010_pattern(D3D12CoreLib::D3D12Core& core, ID3D12Resource* texture, uint32_t width, uint32_t height, uint32_t frameIndex) {
    ID3D12Device* device = core.GetDevice();
    const auto textureDesc = texture->GetDesc();

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    UINT numRows = 0;
    UINT64 rowSize = 0;
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, &numRows, &rowSize, &totalBytes);

    const auto uploadHeap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    const auto uploadDesc = buffer_desc(totalBytes);
    ComPtr<ID3D12Resource> upload;
    throw_if_failed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "Create upload buffer");

    void* mapped = nullptr;
    D3D12_RANGE noRead = { 0, 0 };
    throw_if_failed(upload->Map(0, &noRead, &mapped), "Map upload");
    auto* bytes = static_cast<uint8_t*>(mapped);
    std::fill(bytes, bytes + static_cast<size_t>(totalBytes), 0);

    // P010 stores 10-bit components in the high bits of each 16-bit word.
    constexpr uint16_t uvNeutral = static_cast<uint16_t>(512u << 6);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = bytes + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
        for (uint32_t x = 0; x < width; ++x) {
            const uint16_t y10 = static_cast<uint16_t>(64u + ((x + y + frameIndex * 9u) % 768u));
            write_u16(row + static_cast<size_t>(x) * 2u, static_cast<uint16_t>(y10 << 6));
        }
    }
    for (uint32_t y = 0; y < height / 2; ++y) {
        uint8_t* row = bytes + footprint.Offset + static_cast<size_t>(height + y) * footprint.Footprint.RowPitch;
        for (uint32_t x = 0; x < width; x += 2) {
            write_u16(row + static_cast<size_t>(x) * 2u, uvNeutral);
            write_u16(row + static_cast<size_t>(x) * 2u + 2u, uvNeutral);
        }
    }
    upload->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    throw_if_failed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator");
    throw_if_failed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "CreateCommandList");

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = texture;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;

    commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    throw_if_failed(commandList->Close(), "Close upload command list");
    ID3D12CommandList* lists[] = { commandList.Get() };
    core.GetDirectCommandQueue()->ExecuteCommandLists(1, lists);
    wait_for_queue(device, core.GetDirectCommandQueue());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        const std::wstring outputPath = (argc >= 2) ? argv[1] : L"d3d12video_hevc_p010_sample.mp4";

        D3D12CoreLib::D3D12CoreConfig cfg;
        cfg.enableDebugLayer = false;
        cfg.enableInfoQueue = false;
        cfg.enableDred = false;
        cfg.allowWarpAdapter = false;
        auto core = D3D12CoreLib::D3D12Core::CreateShared(cfg);

        constexpr uint32_t width = 1280;
        constexpr uint32_t height = 720;
        constexpr uint32_t fps = 60;
        constexpr uint32_t frames = 180;

        const auto cap = D3D12VideoEncoder::QueryD3D12VideoEncodeSupport(core.get(), VideoCodec::HEVC, VideoPixelFormat::P010, width, height);
        std::cout << "HEVC/P010 native D3D12 Video Encode supported=" << cap.supported << " message=" << cap.message << "\n";
        if (!cap.supported) {
            std::cout << "Native D3D12 Video Encode HEVC/P010 is not supported on this device.\n";
            return 0;
        }

        ID3D12Device* device = core->GetDevice();
        const auto defaultHeap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        const auto texDesc = p010_texture_desc(width, height);
        ComPtr<ID3D12Resource> texture;
        throw_if_failed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)), "Create P010 texture");

        D3D12VideoEncoderDesc desc;
        desc.outputPath = outputPath;
        desc.width = width;
        desc.height = height;
        desc.frameRateNum = fps;
        desc.frameRateDen = 1;
        desc.backend = D3DVideoEncoderBackendType::D3D12VideoEncode;
        desc.codec = VideoCodec::HEVC;
        desc.internalFormat = VideoPixelFormat::P010;
        desc.bitrate = 16'000'000;
        desc.gopLength = 60;
        desc.bFrameCount = 0;
        desc.asyncMode = false;
        desc.input.core = core.get();
        desc.input.inputFormat = DXGI_FORMAT_P010;
        desc.input.restoreStateAfterEncode = true;
        desc.enableDebugLog = true;

        D3D12VideoEncoder encoder(desc);
        for (uint32_t f = 0; f < frames; ++f) {
            upload_p010_pattern(*core, texture.Get(), width, height, f);
            encoder.write(texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        }
        encoder.close();
        std::wcout << L"wrote: " << outputPath << L"\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        std::cerr << "If this fails while writing .mp4/.mkv due to missing VPS/SPS/PPS, try .h265 output on this driver.\n";
        return 1;
    }
}
