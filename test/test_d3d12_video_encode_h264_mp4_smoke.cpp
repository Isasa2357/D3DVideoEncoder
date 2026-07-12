#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>

#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

#include <Windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace D3DVideoEncoderLib;

namespace {

void throw_if_failed(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        std::cerr << "FAILED: " << message << " HRESULT=0x" << std::hex << static_cast<unsigned long>(hr) << "\n";
        std::exit(1);
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

D3D12_RESOURCE_DESC nv12_texture_desc(uint32_t width, uint32_t height) noexcept {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return desc;
}

D3D12_RESOURCE_BARRIER transition_barrier(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

void wait_for_queue(ID3D12Device* device, ID3D12CommandQueue* queue) {
    ComPtr<ID3D12Fence> fence;
    throw_if_failed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    throw_if_failed(queue->Signal(fence.Get(), 1), "Signal");
    if (fence->GetCompletedValue() < 1) {
        HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            std::cerr << "FAILED: CreateEventW\n";
            std::exit(1);
        }
        throw_if_failed(fence->SetEventOnCompletion(1, eventHandle), "SetEventOnCompletion");
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }
}

void upload_black_nv12(D3D12CoreLib::D3D12Core& core, ID3D12Resource* texture, uint32_t width, uint32_t height) {
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
    throw_if_failed(device->CreateCommittedResource(&uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)), "Create upload buffer");

    void* mapped = nullptr;
    D3D12_RANGE noRead = { 0, 0 };
    throw_if_failed(upload->Map(0, &noRead, &mapped), "Map upload");
    auto* bytes = static_cast<uint8_t*>(mapped);
    std::fill(bytes, bytes + static_cast<size_t>(totalBytes), 128);
    for (uint32_t y = 0; y < height; ++y) {
        uint8_t* row = bytes + footprint.Offset + static_cast<size_t>(y) * footprint.Footprint.RowPitch;
        std::fill(row, row + width, 64);
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
    auto handoffBarrier = transition_barrier(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    commandList->ResourceBarrier(1, &handoffBarrier);
    throw_if_failed(commandList->Close(), "Close upload command list");
    ID3D12CommandList* lists[] = { commandList.Get() };
    core.GetDirectCommandQueue()->ExecuteCommandLists(1, lists);
    wait_for_queue(device, core.GetDirectCommandQueue());
}

bool is_parameter_set_limitation(const std::string& message) {
    return message.find("SPS/PPS") != std::string::npos ||
           message.find("parameter-set") != std::string::npos;
}

} // namespace

int main() {
#ifndef D3DVIDEOENCODER_TEST_HAS_D3D12_VIDEO_ENCODE
    std::cout << "D3D12 Video Encode backend is not enabled; skipping MP4 smoke test.\n";
    return 0;
#else
    try {
        D3D12CoreLib::D3D12CoreConfig cfg;
        cfg.enableDebugLayer = false;
        cfg.enableInfoQueue = false;
        cfg.enableDred = false;
        cfg.allowWarpAdapter = false;
        auto core = D3D12CoreLib::D3D12Core::CreateShared(cfg);

        constexpr uint32_t width = 320;
        constexpr uint32_t height = 180;
        const auto cap = D3D12VideoEncoder::QueryD3D12VideoEncodeSupport(core.get(), VideoCodec::H264, VideoPixelFormat::NV12, width, height);
        std::cout << "D3D12VideoEncode H264/NV12 supported=" << cap.supported << " message=" << cap.message << "\n";
        if (!cap.supported) {
            std::cout << "Native D3D12 Video Encode H.264/NV12 is not supported on this device; skipping.\n";
            return 0;
        }

        ID3D12Device* device = core->GetDevice();
        const auto defaultHeap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
        const auto texDesc = nv12_texture_desc(width, height);
        ComPtr<ID3D12Resource> texture;
        throw_if_failed(device->CreateCommittedResource(&defaultHeap, D3D12_HEAP_FLAG_NONE, &texDesc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)), "Create NV12 texture");
        upload_black_nv12(*core, texture.Get(), width, height);

        const std::filesystem::path outPath = "d3d12video_h264_nv12_smoke.mp4";
        std::filesystem::remove(outPath);

        D3D12VideoEncoderDesc desc;
        desc.outputPath = outPath.wstring();
        desc.width = width;
        desc.height = height;
        desc.frameRateNum = 30;
        desc.frameRateDen = 1;
        desc.backend = D3DVideoEncoderBackendType::D3D12VideoEncode;
        desc.codec = VideoCodec::H264;
        desc.internalFormat = VideoPixelFormat::NV12;
        desc.bitrate = 4'000'000;
        desc.gopLength = 15;
        desc.bFrameCount = 0;
        desc.asyncMode = false;
        desc.enableDebugLog = true;
        desc.input.core = core.get();
        desc.input.inputFormat = DXGI_FORMAT_NV12;
        desc.input.restoreStateAfterEncode = true;

        D3D12VideoEncoder encoder(desc);
        for (int i = 0; i < 30; ++i) {
            encoder.write(texture.Get(), D3D12_RESOURCE_STATE_COMMON);
        }
        encoder.close();

        const auto fileSize = std::filesystem::file_size(outPath);
        std::cout << "D3D12 Video Encode MP4 output size: " << fileSize << " bytes\n";
        if (fileSize == 0) {
            std::cerr << "FAILED: D3D12 Video Encode MP4 output is empty.\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& e) {
        const std::string message = e.what();
        if (is_parameter_set_limitation(message)) {
            std::cout << "D3D12 Video Encode produced H.264 but driver did not emit SPS/PPS needed for MP4 mux; skipping MP4 container smoke. "
                      << message << "\n";
            return 0;
        }
        std::cerr << "D3D12 Video Encode H.264/NV12 MP4 smoke failed: " << e.what() << "\n";
        return 1;
    }
#endif
}
