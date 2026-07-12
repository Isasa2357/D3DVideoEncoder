#include "test_config.hpp"

#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>

#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace D3DVideoEncoderLib;

namespace {

struct Options {
    std::filesystem::path input;
    std::filesystem::path output;
    std::string mode = "direct";
    uint32_t frames = 30;
    bool async = false;
    bool debugLayer = false;
    bool closeTwice = false;
    bool flushBeforeClose = false;
    uint32_t repeatOpenCount = 1;
};

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), out.data(), count, nullptr, nullptr);
    return out;
}

Options parse_args(int argc, wchar_t** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto value = [&]() -> const wchar_t* {
            if (++i >= argc) throw std::runtime_error("Missing argument value.");
            return argv[i];
        };
        if (arg == L"--input") options.input = value();
        else if (arg == L"--output") options.output = value();
        else if (arg == L"--mode") options.mode = narrow(value());
        else if (arg == L"--frames") options.frames = static_cast<uint32_t>(std::wcstoul(value(), nullptr, 10));
        else if (arg == L"--repeat-open-count") options.repeatOpenCount = static_cast<uint32_t>(std::wcstoul(value(), nullptr, 10));
        else if (arg == L"--async") options.async = true;
        else if (arg == L"--debug-layer") options.debugLayer = true;
        else if (arg == L"--close-twice") options.closeTwice = true;
        else if (arg == L"--flush-before-close") options.flushBeforeClose = true;
        else throw std::runtime_error("Unknown argument: " + narrow(arg));
    }
    if (options.output.empty()) throw std::runtime_error("--output is required.");
    if (options.frames == 0 || options.repeatOpenCount == 0) throw std::runtime_error("frame/repeat count must be non-zero.");
    if (options.mode != "direct" && options.mode != "bgra") throw std::runtime_error("--mode must be direct or bgra.");
    return options;
}

void throw_if_failed(HRESULT hr, const char* operation) {
    if (FAILED(hr)) throw std::runtime_error(std::string(operation) + " failed.");
}

D3D12_HEAP_PROPERTIES heap_properties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES out = {};
    out.Type = type;
    out.CreationNodeMask = 1;
    out.VisibleNodeMask = 1;
    return out;
}

D3D12_RESOURCE_DESC buffer_desc(uint64_t size) {
    D3D12_RESOURCE_DESC out = {};
    out.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    out.Width = size;
    out.Height = 1;
    out.DepthOrArraySize = 1;
    out.MipLevels = 1;
    out.SampleDesc.Count = 1;
    out.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return out;
}

D3D12_RESOURCE_DESC texture_desc(uint32_t width, uint32_t height, DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC out = {};
    out.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    out.Width = width;
    out.Height = height;
    out.DepthOrArraySize = 1;
    out.MipLevels = 1;
    out.Format = format;
    out.SampleDesc.Count = 1;
    return out;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER out = {};
    out.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    out.Transition.pResource = resource;
    out.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    out.Transition.StateBefore = before;
    out.Transition.StateAfter = after;
    return out;
}

void wait_for_queue(ID3D12Device* device, ID3D12CommandQueue* queue) {
    ComPtr<ID3D12Fence> fence;
    throw_if_failed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence(upload)");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) throw std::runtime_error("CreateEvent(upload) failed.");
    try {
        throw_if_failed(queue->Signal(fence.Get(), 1), "Signal(upload)");
        if (fence->GetCompletedValue() < 1) {
            throw_if_failed(fence->SetEventOnCompletion(1, event), "SetEventOnCompletion(upload)");
            if (WaitForSingleObject(event, 30000) != WAIT_OBJECT_0) throw std::runtime_error("Upload queue wait failed.");
        }
    } catch (...) {
        CloseHandle(event);
        throw;
    }
    CloseHandle(event);
}

ComPtr<ID3D12Resource> create_texture(ID3D12Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    const auto heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    const auto desc = texture_desc(width, height, format);
    ComPtr<ID3D12Resource> texture;
    throw_if_failed(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)),
        "CreateCommittedResource(input texture)");
    return texture;
}

void upload_frame(
    D3D12CoreLib::D3D12Core& core,
    ID3D12Resource* texture,
    D3D12_RESOURCE_STATES& state,
    DXGI_FORMAT format,
    const uint8_t* frame,
    uint32_t width,
    uint32_t height) {
    ID3D12Device* device = core.GetDevice();
    const UINT subresources = format == DXGI_FORMAT_NV12 ? 2u : 1u;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2] = {};
    UINT rows[2] = {};
    UINT64 rowSizes[2] = {};
    UINT64 totalBytes = 0;
    const auto textureDesc = texture->GetDesc();
    device->GetCopyableFootprints(&textureDesc, 0, subresources, 0, footprints, rows, rowSizes, &totalBytes);

    const auto uploadHeap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    const auto uploadDesc = buffer_desc(totalBytes);
    ComPtr<ID3D12Resource> upload;
    throw_if_failed(device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)),
        "CreateCommittedResource(upload)");

    void* mapped = nullptr;
    const D3D12_RANGE noRead = {0, 0};
    throw_if_failed(upload->Map(0, &noRead, &mapped), "Map(upload)");
    auto* bytes = static_cast<uint8_t*>(mapped);
    std::fill(bytes, bytes + static_cast<size_t>(totalBytes), 128);
    if (format == DXGI_FORMAT_NV12) {
        for (uint32_t y = 0; y < height; ++y) {
            std::copy_n(frame + static_cast<size_t>(y) * width, width,
                        bytes + footprints[0].Offset + static_cast<size_t>(y) * footprints[0].Footprint.RowPitch);
        }
        const uint8_t* uv = frame + static_cast<size_t>(width) * height;
        for (uint32_t y = 0; y < height / 2; ++y) {
            std::copy_n(uv + static_cast<size_t>(y) * width, width,
                        bytes + footprints[1].Offset + static_cast<size_t>(y) * footprints[1].Footprint.RowPitch);
        }
    } else {
        const uint32_t rowBytes = width * 4;
        for (uint32_t y = 0; y < height; ++y) {
            std::copy_n(frame + static_cast<size_t>(y) * rowBytes, rowBytes,
                        bytes + footprints[0].Offset + static_cast<size_t>(y) * footprints[0].Footprint.RowPitch);
        }
    }
    upload->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    throw_if_failed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "CreateCommandAllocator(upload)");
    throw_if_failed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "CreateCommandList(upload)");
    if (state != D3D12_RESOURCE_STATE_COPY_DEST) {
        const auto barrier = transition(texture, state, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &barrier);
    }
    for (UINT subresource = 0; subresource < subresources; ++subresource) {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = subresource;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[subresource];
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    const auto handoff = transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(1, &handoff);
    state = D3D12_RESOURCE_STATE_COMMON;
    throw_if_failed(list->Close(), "Close(upload)");
    ID3D12CommandList* lists[] = {list.Get()};
    core.GetDirectCommandQueue()->ExecuteCommandLists(1, lists);
    wait_for_queue(device, core.GetDirectCommandQueue());
}

DWORD handle_count() {
    DWORD value = 0;
    GetProcessHandleCount(GetCurrentProcess(), &value);
    return value;
}

SIZE_T working_set() {
    using Fn = BOOL(WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    auto* proc = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetProcessMemoryInfo"));
    PROCESS_MEMORY_COUNTERS counters = {};
    counters.cb = sizeof(counters);
    return proc && proc(GetCurrentProcess(), &counters, sizeof(counters)) ? counters.WorkingSetSize : 0;
}

void print_debug_messages(ID3D12Device* device) {
    ComPtr<ID3D12InfoQueue> queue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) return;
    std::cout << "Debug Layer stored messages: " << queue->GetNumStoredMessagesAllowedByRetrievalFilter() << "\n";
    for (UINT64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0;
        queue->GetMessage(i, nullptr, &size);
        std::vector<uint8_t> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if (SUCCEEDED(queue->GetMessage(i, message, &size))) {
            std::cerr << "D3D12 message severity=" << message->Severity << " id=" << message->ID
                      << " text=" << (message->pDescription ? message->pDescription : "") << "\n";
        }
    }
}

std::filesystem::path output_for_iteration(const std::filesystem::path& output, uint32_t iteration, uint32_t count) {
    if (count == 1) return output;
    return output.parent_path() / (output.stem().wstring() + L"_iter" + std::to_wstring(iteration) + output.extension().wstring());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        constexpr uint32_t width = 640;
        constexpr uint32_t height = 360;
        constexpr uint32_t fps = 30;
        const Options options = parse_args(argc, argv);
        const DXGI_FORMAT format = options.mode == "bgra" ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_NV12;
        const size_t frameSize = format == DXGI_FORMAT_NV12
            ? static_cast<size_t>(width) * height * 3 / 2
            : static_cast<size_t>(width) * height * 4;

        D3D12CoreLib::D3D12CoreConfig config;
        config.enableDebugLayer = options.debugLayer;
        config.enableInfoQueue = options.debugLayer;
        config.enableDred = true;
        config.allowWarpAdapter = false;
        auto core = D3D12CoreLib::D3D12Core::CreateShared(config);
        const auto capability = D3D12VideoEncoder::QueryNvencSupport(core.get(), VideoCodec::H264, VideoPixelFormat::NV12);
        if (!capability.supported) throw std::runtime_error("NVENC D3D12 H.264/NV12 is unsupported: " + capability.message);

        std::cout << "NVENC D3D12 acceptance\n";
        std::cout << "  adapter: " << narrow(core->DeviceContext().GetAdapterName()) << "\n";
        const LUID luid = core->GetDevice()->GetAdapterLuid();
        std::cout << "  LUID: 0x" << std::hex << static_cast<uint32_t>(luid.HighPart) << ":0x" << luid.LowPart << std::dec << "\n";
        std::cout << "  mode: " << options.mode << " async=" << options.async << " frames=" << options.frames << "\n";
        std::cout << "  working set start: " << working_set() << " handles start: " << handle_count() << "\n";

        std::filesystem::create_directories(options.output.parent_path());
        for (uint32_t iteration = 0; iteration < options.repeatOpenCount; ++iteration) {
            const auto output = output_for_iteration(options.output, iteration, options.repeatOpenCount);
            std::error_code ec;
            std::filesystem::remove(output, ec);

            std::ifstream input;
            if (!options.input.empty()) {
                input.open(options.input, std::ios::binary);
                if (!input) throw std::runtime_error("Failed to open raw input.");
            }
            std::vector<uint8_t> frame(frameSize);

            D3D12VideoEncoderDesc desc;
            desc.outputPath = output.wstring();
            desc.width = width;
            desc.height = height;
            desc.frameRateNum = fps;
            desc.frameRateDen = 1;
            desc.backend = D3DVideoEncoderBackendType::NvencD3D12;
            desc.codec = VideoCodec::H264;
            desc.internalFormat = VideoPixelFormat::NV12;
            desc.bitrate = 4'000'000;
            desc.gopLength = 15;
            desc.asyncMode = options.async;
            desc.queueDepth = 2;
            desc.queueFullPolicy = EncoderQueueFullPolicy::Block;
            desc.enableDebugLog = true;
            desc.input.core = core.get();
            desc.input.inputFormat = format;
            desc.input.allowFormatConversion = format != DXGI_FORMAT_NV12;
            desc.input.processingShaderDirectory = D3DVIDEOENCODER_TEST_D3D12_SHADER_DIR;
            desc.input.restoreStateAfterEncode = true;

            D3D12VideoEncoder encoder(desc);
            std::vector<ComPtr<ID3D12Resource>> asyncTextures;
            if (options.async) asyncTextures.reserve(options.frames);
            ComPtr<ID3D12Resource> syncTexture;
            D3D12_RESOURCE_STATES syncState = D3D12_RESOURCE_STATE_COPY_DEST;
            if (!options.async) syncTexture = create_texture(core->GetDevice(), width, height, format);

            for (uint32_t index = 0; index < options.frames; ++index) {
                if (input) {
                    input.read(reinterpret_cast<char*>(frame.data()), static_cast<std::streamsize>(frame.size()));
                    if (input.gcount() != static_cast<std::streamsize>(frame.size())) throw std::runtime_error("Raw input is shorter than requested frames.");
                } else {
                    std::fill(frame.begin(), frame.end(), 128);
                    for (size_t i = 0; i < std::min<size_t>(frame.size(), static_cast<size_t>(width) * height); ++i) {
                        frame[i] = static_cast<uint8_t>(32 + ((i + index * 7u) & 0x7fu));
                    }
                }

                ComPtr<ID3D12Resource> texture = syncTexture;
                D3D12_RESOURCE_STATES state = syncState;
                if (options.async) texture = create_texture(core->GetDevice(), width, height, format);
                upload_frame(*core, texture.Get(), state, format, frame.data(), width, height);
                encoder.write(texture.Get(), state, static_cast<int64_t>(index) * (10'000'000 / fps));
                if (options.async) asyncTextures.push_back(texture);
                else syncState = state;
            }

            if (options.flushBeforeClose) {
                encoder.flush();
                std::cout << "  iteration " << iteration << " flush: success\n";
            }
            encoder.close();
            std::cout << "  iteration " << iteration << " close: success sent=" << options.frames
                      << " written=" << encoder.writtenFrameCount() << "\n";
            if (options.closeTwice) {
                encoder.close();
                std::cout << "  iteration " << iteration << " second close: success\n";
            }
            const uintmax_t size = std::filesystem::exists(output) ? std::filesystem::file_size(output) : 0;
            std::cout << "  output=" << output.string() << " size=" << size << "\n";
            if (encoder.writtenFrameCount() != options.frames || size == 0) throw std::runtime_error("NVENC D3D12 output validation failed.");
        }

        if (options.debugLayer) print_debug_messages(core->GetDevice());
        std::cout << "  working set end: " << working_set() << " handles end: " << handle_count() << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "NVENC D3D12 acceptance failed: " << error.what() << "\n";
        return 1;
    }
}
