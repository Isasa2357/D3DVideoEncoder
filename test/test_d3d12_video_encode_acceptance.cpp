#include "test_config.hpp"

#include <D3DVideoEncoder/D3D12VideoEncoder.hpp>
#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

#include <Windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <psapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace D3DVideoEncoderLib;

namespace {

struct Options {
    std::string mode = "direct";
    std::filesystem::path output;
    std::filesystem::path input;
    std::string inputFormat;
    uint32_t frames = 30;
    uint32_t gopLength = 15;
    uint32_t diagnosticTimeoutSeconds = 0;
    bool async = false;
    bool debugLayer = false;
    bool handlePlateau = false;
    bool closeTwice = false;
    bool flushBeforeClose = false;
    bool streamInput = false;
    uint32_t repeatOpenCount = 1;
};

struct RawInput {
    std::vector<uint8_t> bytes;
    std::filesystem::path path;
    size_t frameSize = 0;
    bool streaming = false;
};

void throw_if_failed(HRESULT hr, const char* message) {
    if (FAILED(hr)) {
        std::ostringstream oss;
        oss << message << " failed. HRESULT=0x" << std::hex << static_cast<unsigned long>(hr);
        throw std::runtime_error(oss.str());
    }
}

std::string hr_hex(HRESULT hr) {
    std::ostringstream os;
    os << "0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    return os.str();
}

std::string luid_hex(LUID luid) {
    std::ostringstream os;
    os << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0')
       << static_cast<uint32_t>(luid.HighPart)
       << ":0x" << std::setw(8) << static_cast<uint32_t>(luid.LowPart);
    return os.str();
}

std::string resource_state_name(D3D12_RESOURCE_STATES state) {
    switch (state) {
    case D3D12_RESOURCE_STATE_COMMON: return "D3D12_RESOURCE_STATE_COMMON";
    case D3D12_RESOURCE_STATE_COPY_SOURCE: return "D3D12_RESOURCE_STATE_COPY_SOURCE";
    case D3D12_RESOURCE_STATE_COPY_DEST: return "D3D12_RESOURCE_STATE_COPY_DEST";
    case D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ: return "D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ";
    case D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE: return "D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE";
    default: {
        std::ostringstream os;
        os << "D3D12_RESOURCE_STATES(0x" << std::hex << std::uppercase << static_cast<unsigned long>(state) << ")";
        return os.str();
    }
    }
}

std::string narrow(const std::wstring& value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required <= 0) return {};
    std::string out(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        out.data(),
        required,
        nullptr,
        nullptr);
    return out;
}

const char* breadcrumb_op_name(D3D12_AUTO_BREADCRUMB_OP op) noexcept {
    switch (op) {
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return "RESOURCEBARRIER";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return "COPYBUFFERREGION";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return "COPYTEXTUREREGION";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return "BEGINSUBMISSION";
    case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return "ENDSUBMISSION";
    case D3D12_AUTO_BREADCRUMB_OP_ENCODEFRAME: return "ENCODEFRAME";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVEENCODEROUTPUTMETADATA: return "RESOLVEENCODEROUTPUTMETADATA";
    case D3D12_AUTO_BREADCRUMB_OP_BEGIN_COMMAND_LIST: return "BEGIN_COMMAND_LIST";
    default: return "OTHER";
    }
}

void print_dred_allocation_chain(const D3D12_DRED_ALLOCATION_NODE1* node, const char* label) {
    std::cerr << "DRED " << label << " allocations:";
    if (!node) {
        std::cerr << " none\n";
        return;
    }
    std::cerr << "\n";
    uint32_t count = 0;
    for (auto* it = node; it && count < 8; it = it->pNext, ++count) {
        std::cerr << "  [" << count << "] type=" << static_cast<unsigned>(it->AllocationType)
                  << " object=" << it->pObject
                  << " nameA=" << (it->ObjectNameA ? it->ObjectNameA : "")
                  << "\n";
    }
    if (node && count == 8) {
        std::cerr << "  ... truncated\n";
    }
}

void print_device_removed_and_dred(ID3D12Device* device) {
    if (!device) return;

    const HRESULT removed = device->GetDeviceRemovedReason();
    std::cerr << "GetDeviceRemovedReason: " << hr_hex(removed) << "\n";

    ComPtr<ID3D12DeviceRemovedExtendedData2> dred2;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dred2));
    if (SUCCEEDED(hr) && dred2) {
        std::cerr << "DRED device state: " << static_cast<unsigned>(dred2->GetDeviceState()) << "\n";

        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {};
        hr = dred2->GetAutoBreadcrumbsOutput1(&breadcrumbs);
        std::cerr << "DRED auto-breadcrumbs hr=" << hr_hex(hr) << "\n";
        uint32_t nodeIndex = 0;
        for (auto* node = breadcrumbs.pHeadAutoBreadcrumbNode; node && nodeIndex < 8; node = node->pNext, ++nodeIndex) {
            const UINT last = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            const UINT lastCommandIndex = (last < node->BreadcrumbCount) ? last : (node->BreadcrumbCount ? node->BreadcrumbCount - 1 : 0);
            const D3D12_AUTO_BREADCRUMB_OP lastOp =
                (node->pCommandHistory && node->BreadcrumbCount > 0) ? node->pCommandHistory[lastCommandIndex] : D3D12_AUTO_BREADCRUMB_OP_SETMARKER;
            std::cerr << "  breadcrumb[" << nodeIndex << "]"
                      << " list=" << node->pCommandList
                      << " queue=" << node->pCommandQueue
                      << " count=" << node->BreadcrumbCount
                      << " last=" << last
                      << " lastOp=" << breadcrumb_op_name(lastOp)
                      << "(" << static_cast<unsigned>(lastOp) << ")"
                      << " listNameA=" << (node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "")
                      << " queueNameA=" << (node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "")
                      << "\n";
        }

        D3D12_DRED_PAGE_FAULT_OUTPUT2 pageFault = {};
        hr = dred2->GetPageFaultAllocationOutput2(&pageFault);
        std::cerr << "DRED page-fault hr=" << hr_hex(hr)
                  << " VA=0x" << std::hex << std::uppercase << pageFault.PageFaultVA << std::dec
                  << " flags=0x" << std::hex << std::uppercase << static_cast<unsigned>(pageFault.PageFaultFlags) << std::dec
                  << "\n";
        print_dred_allocation_chain(pageFault.pHeadExistingAllocationNode, "existing");
        print_dred_allocation_chain(pageFault.pHeadRecentFreedAllocationNode, "recent freed");
        return;
    }

    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    hr = device->QueryInterface(IID_PPV_ARGS(&dred));
    if (FAILED(hr) || !dred) {
        std::cerr << "DRED interface unavailable hr=" << hr_hex(hr) << "\n";
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
    hr = dred->GetAutoBreadcrumbsOutput(&breadcrumbs);
    std::cerr << "DRED auto-breadcrumbs hr=" << hr_hex(hr)
              << " head=" << breadcrumbs.pHeadAutoBreadcrumbNode << "\n";
    D3D12_DRED_PAGE_FAULT_OUTPUT pageFault = {};
    hr = dred->GetPageFaultAllocationOutput(&pageFault);
    std::cerr << "DRED page-fault hr=" << hr_hex(hr)
              << " VA=0x" << std::hex << std::uppercase << pageFault.PageFaultVA << std::dec
              << "\n";
}

void print_adapter_identity(ID3D12Device* device, const std::string& helperAdapterName) {
    if (!device) return;
    const LUID deviceLuid = device->GetAdapterLuid();
    std::cout << "  ID3D12Device::GetAdapterLuid: " << luid_hex(deviceLuid) << "\n";

    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    std::cout << "  CreateDXGIFactory2: " << hr_hex(hr) << "\n";
    if (FAILED(hr) || !factory) return;

    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapterByLuid(deviceLuid, IID_PPV_ARGS(&adapter));
    std::cout << "  IDXGIFactory4::EnumAdapterByLuid(current LUID): " << hr_hex(hr) << "\n";
    if (FAILED(hr) || !adapter) return;

    DXGI_ADAPTER_DESC1 desc = {};
    hr = adapter->GetDesc1(&desc);
    std::cout << "  IDXGIAdapter1::GetDesc1: " << hr_hex(hr) << "\n";
    if (FAILED(hr)) return;

    const std::string dxgiName = narrow(desc.Description);
    const bool hardware = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0;
    std::cout << "  adapter name from D3D12Helper: " << helperAdapterName << "\n";
    std::cout << "  adapter name from EnumAdapterByLuid: " << dxgiName << "\n";
    std::cout << "  VendorId: 0x" << std::hex << std::uppercase << desc.VendorId << std::dec << "\n";
    std::cout << "  DeviceId: 0x" << std::hex << std::uppercase << desc.DeviceId << std::dec << "\n";
    std::cout << "  hardware adapter: " << (hardware ? "yes" : "no") << "\n";
}

DXGI_FORMAT input_format_from_options(const Options& options) {
    const std::string format = options.inputFormat.empty() ? options.mode : options.inputFormat;
    if (format == "direct" || format == "nv12") return DXGI_FORMAT_NV12;
    if (format == "rgba") return DXGI_FORMAT_R8G8B8A8_UNORM;
    if (format == "bgra") return DXGI_FORMAT_B8G8R8A8_UNORM;
    throw std::runtime_error("--input-format must be nv12, rgba, or bgra.");
}

const char* input_format_name(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_NV12: return "NV12";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    default: return "unsupported";
    }
}

size_t raw_frame_size(DXGI_FORMAT format, uint32_t width, uint32_t height) {
    switch (format) {
    case DXGI_FORMAT_NV12:
        return static_cast<size_t>(width) * height * 3 / 2;
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
        return static_cast<size_t>(width) * height * 4;
    default:
        throw std::runtime_error("Unsupported raw input format.");
    }
}

RawInput load_raw_input(const Options& options, DXGI_FORMAT format, uint32_t width, uint32_t height) {
    RawInput input;
    input.frameSize = raw_frame_size(format, width, height);
    if (options.input.empty()) {
        return input;
    }

    const uint64_t requiredBytes = static_cast<uint64_t>(input.frameSize) * options.frames;
    const auto size = std::filesystem::file_size(options.input);
    if (size < requiredBytes) {
        std::ostringstream oss;
        oss << "Raw input is too small. path=" << options.input.string()
            << " size=" << size
            << " required=" << requiredBytes;
        throw std::runtime_error(oss.str());
    }
    if (options.streamInput) {
        input.path = options.input;
        input.streaming = true;
        return input;
    }

    input.bytes.resize(static_cast<size_t>(requiredBytes));
    std::ifstream file(options.input, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to open raw input: " + options.input.string());
    }
    file.read(reinterpret_cast<char*>(input.bytes.data()), static_cast<std::streamsize>(input.bytes.size()));
    if (file.gcount() != static_cast<std::streamsize>(input.bytes.size())) {
        throw std::runtime_error("Failed to read requested raw input bytes: " + options.input.string());
    }
    return input;
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

D3D12_RESOURCE_DESC texture_desc(uint32_t width, uint32_t height, DXGI_FORMAT format) noexcept {
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
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

struct UploadStateTrace {
    D3D12_RESOURCE_STATES beforeUpload = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_STATES afterCopy = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_STATES afterHandoff = D3D12_RESOURCE_STATE_COPY_DEST;
};

void wait_for_queue(ID3D12Device* device, ID3D12CommandQueue* queue) {
    ComPtr<ID3D12Fence> fence;
    throw_if_failed(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "CreateFence");
    throw_if_failed(queue->Signal(fence.Get(), 1), "Signal");
    if (fence->GetCompletedValue() < 1) {
        HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!eventHandle) {
            throw std::runtime_error("CreateEventW failed while waiting for upload queue.");
        }
        throw_if_failed(fence->SetEventOnCompletion(1, eventHandle), "SetEventOnCompletion");
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }
}

void print_info_queue(ID3D12Device* device) {
    ComPtr<ID3D12InfoQueue> infoQueue;
    const HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&infoQueue));
    if (FAILED(hr) || !infoQueue) {
        std::cerr << "Debug Layer messages: ID3D12InfoQueue unavailable hr=" << hr_hex(hr) << "\n";
        return;
    }

    const UINT64 count = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    std::cerr << "Debug Layer stored messages: " << count << "\n";
    const UINT64 first = (count > 16) ? count - 16 : 0;
    for (UINT64 i = first; i < count; ++i) {
        SIZE_T length = 0;
        HRESULT msgHr = infoQueue->GetMessage(i, nullptr, &length);
        if (FAILED(msgHr) || length == 0) {
            std::cerr << "  [" << i << "] GetMessage length failed hr=" << hr_hex(msgHr) << "\n";
            continue;
        }
        std::vector<uint8_t> storage(length);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        msgHr = infoQueue->GetMessage(i, message, &length);
        if (FAILED(msgHr)) {
            std::cerr << "  [" << i << "] GetMessage failed hr=" << hr_hex(msgHr) << "\n";
            continue;
        }
        std::cerr << "  [" << i << "] severity=" << static_cast<unsigned>(message->Severity)
                  << " id=" << message->ID
                  << " category=" << static_cast<unsigned>(message->Category)
                  << " desc=" << (message->pDescription ? message->pDescription : "")
                  << "\n";
    }
}

DWORD current_handle_count() {
    DWORD count = 0;
    if (!GetProcessHandleCount(GetCurrentProcess(), &count)) {
        return 0;
    }
    return count;
}

SIZE_T current_working_set_bytes() {
    using GetProcessMemoryInfoFn = BOOL(WINAPI*)(HANDLE, PPROCESS_MEMORY_COUNTERS, DWORD);
    static GetProcessMemoryInfoFn getProcessMemoryInfo = []() -> GetProcessMemoryInfoFn {
        if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
            if (auto* proc = GetProcAddress(kernel, "K32GetProcessMemoryInfo")) {
                return reinterpret_cast<GetProcessMemoryInfoFn>(proc);
            }
        }
        if (HMODULE psapi = LoadLibraryW(L"Psapi.dll")) {
            if (auto* proc = GetProcAddress(psapi, "GetProcessMemoryInfo")) {
                return reinterpret_cast<GetProcessMemoryInfoFn>(proc);
            }
        }
        return nullptr;
    }();

    PROCESS_MEMORY_COUNTERS counters = {};
    counters.cb = sizeof(counters);
    if (!getProcessMemoryInfo ||
        !getProcessMemoryInfo(GetCurrentProcess(), &counters, static_cast<DWORD>(sizeof(counters)))) {
        return 0;
    }
    return counters.WorkingSetSize;
}

ComPtr<ID3D12Resource> create_texture(
    D3D12CoreLib::D3D12Core& core,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format) {

    const auto defaultHeap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    const auto desc = texture_desc(width, height, format);
    ComPtr<ID3D12Resource> texture;
    throw_if_failed(core.GetDevice()->CreateCommittedResource(
        &defaultHeap,
        D3D12_HEAP_FLAG_NONE,
        &desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&texture)), "Create input texture");
    return texture;
}

void upload_frame(
    D3D12CoreLib::D3D12Core& core,
    ID3D12Resource* texture,
    D3D12_RESOURCE_STATES& textureState,
    uint32_t width,
    uint32_t height,
    DXGI_FORMAT format,
    uint32_t frameIndex,
    const uint8_t* rawFrameData,
    UploadStateTrace& trace) {

    ID3D12Device* device = core.GetDevice();
    const auto textureDesc = texture->GetDesc();
    trace.beforeUpload = textureState;

    const bool rawNv12 = rawFrameData && format == DXGI_FORMAT_NV12;
    const UINT subresourceCount = rawNv12 ? 2u : 1u;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2] = {};
    UINT numRows[2] = {};
    UINT64 rowSizes[2] = {};
    UINT64 totalBytes = 0;
    device->GetCopyableFootprints(&textureDesc, 0, subresourceCount, 0, footprints, numRows, rowSizes, &totalBytes);

    const auto uploadHeap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    const auto uploadDesc = buffer_desc(totalBytes);
    ComPtr<ID3D12Resource> upload;
    throw_if_failed(device->CreateCommittedResource(
        &uploadHeap,
        D3D12_HEAP_FLAG_NONE,
        &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&upload)), "Create upload buffer");

    void* mapped = nullptr;
    D3D12_RANGE noRead = { 0, 0 };
    throw_if_failed(upload->Map(0, &noRead, &mapped), "Map upload");
    auto* bytes = static_cast<uint8_t*>(mapped);
    std::fill(bytes, bytes + static_cast<size_t>(totalBytes), 128);

    if (rawNv12) {
        if (numRows[0] < height || numRows[1] < height / 2) {
            throw std::runtime_error("NV12 copy footprints do not expose the expected Y/UV plane row counts.");
        }
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* src = rawFrameData + static_cast<size_t>(y) * width;
            uint8_t* dst = bytes + footprints[0].Offset + static_cast<size_t>(y) * footprints[0].Footprint.RowPitch;
            std::copy(src, src + width, dst);
        }
        const uint8_t* uv = rawFrameData + static_cast<size_t>(width) * height;
        for (uint32_t y = 0; y < height / 2; ++y) {
            const uint8_t* src = uv + static_cast<size_t>(y) * width;
            uint8_t* dst = bytes + footprints[1].Offset + static_cast<size_t>(y) * footprints[1].Footprint.RowPitch;
            std::copy(src, src + width, dst);
        }
    } else if (rawFrameData && (format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM)) {
        const uint32_t sourceRowBytes = width * 4;
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* src = rawFrameData + static_cast<size_t>(y) * sourceRowBytes;
            uint8_t* dst = bytes + footprints[0].Offset + static_cast<size_t>(y) * footprints[0].Footprint.RowPitch;
            std::copy(src, src + sourceRowBytes, dst);
        }
    } else if (format == DXGI_FORMAT_NV12) {
        for (uint32_t y = 0; y < height; ++y) {
            uint8_t* row = bytes + footprints[0].Offset + static_cast<size_t>(y) * footprints[0].Footprint.RowPitch;
            for (uint32_t x = 0; x < width; ++x) {
                row[x] = static_cast<uint8_t>(48 + ((x + y + frameIndex * 7u) & 0x3f));
            }
        }
    } else {
        for (uint32_t y = 0; y < height; ++y) {
            uint8_t* row = bytes + footprints[0].Offset + static_cast<size_t>(y) * footprints[0].Footprint.RowPitch;
            for (uint32_t x = 0; x < width; ++x) {
                const size_t p = static_cast<size_t>(x) * 4;
                row[p + 0] = static_cast<uint8_t>((x + frameIndex * 5u) & 0xffu);
                row[p + 1] = static_cast<uint8_t>((y + frameIndex * 3u) & 0xffu);
                row[p + 2] = static_cast<uint8_t>((x + y + frameIndex * 11u) & 0xffu);
                row[p + 3] = 255;
            }
        }
    }
    upload->Unmap(0, nullptr);

    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    throw_if_failed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "Create upload command allocator");
    throw_if_failed(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&commandList)), "Create upload command list");

    if (textureState != D3D12_RESOURCE_STATE_COPY_DEST) {
        auto uploadBarrier = transition_barrier(texture, textureState, D3D12_RESOURCE_STATE_COPY_DEST);
        commandList->ResourceBarrier(1, &uploadBarrier);
        textureState = D3D12_RESOURCE_STATE_COPY_DEST;
    }
    for (UINT subresource = 0; subresource < subresourceCount; ++subresource) {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = texture;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = subresource;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = footprints[subresource];

        commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    trace.afterCopy = D3D12_RESOURCE_STATE_COPY_DEST;
    auto handoffBarrier = transition_barrier(texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    commandList->ResourceBarrier(1, &handoffBarrier);
    textureState = D3D12_RESOURCE_STATE_COMMON;
    trace.afterHandoff = textureState;
    throw_if_failed(commandList->Close(), "Close upload command list");
    ID3D12CommandList* lists[] = { commandList.Get() };
    core.GetDirectCommandQueue()->ExecuteCommandLists(1, lists);
    wait_for_queue(device, core.GetDirectCommandQueue());
}

void run_single_frame_encode_iteration(
    D3D12CoreLib::D3D12Core& core,
    const Options& options,
    const std::filesystem::path& outputPath,
    uint32_t frameIndex) {
    constexpr uint32_t width = 640;
    constexpr uint32_t height = 360;
    constexpr uint32_t frameRateNum = 30;
    constexpr uint32_t frameRateDen = 1;

    const DXGI_FORMAT inputFormat = input_format_from_options(options);
    const bool rgbaInput = inputFormat == DXGI_FORMAT_R8G8B8A8_UNORM || inputFormat == DXGI_FORMAT_B8G8R8A8_UNORM;

    if (outputPath.has_parent_path()) {
        std::filesystem::create_directories(outputPath.parent_path());
    }
    std::filesystem::remove(outputPath);

    D3D12VideoEncoderDesc desc;
    desc.outputPath = outputPath.wstring();
    desc.width = width;
    desc.height = height;
    desc.frameRateNum = frameRateNum;
    desc.frameRateDen = frameRateDen;
    desc.backend = D3DVideoEncoderBackendType::D3D12VideoEncode;
    desc.codec = VideoCodec::H264;
    desc.internalFormat = VideoPixelFormat::NV12;
    desc.bitrate = 4'000'000;
    desc.gopLength = options.gopLength;
    desc.bFrameCount = 0;
    desc.rateControl = VideoRateControlMode::CBR;
    desc.asyncMode = false;
    desc.queueDepth = 2;
    desc.queueFullPolicy = EncoderQueueFullPolicy::Block;
    desc.enableDebugLog = false;
    desc.input.core = &core;
    desc.input.inputFormat = inputFormat;
    desc.input.allowFormatConversion = rgbaInput;
    desc.input.processingShaderDirectory = D3DVIDEOENCODER_TEST_D3D12_SHADER_DIR;
    desc.input.restoreStateAfterEncode = true;

    D3D12VideoEncoder encoder(desc);
    auto texture = create_texture(core, width, height, desc.input.inputFormat);
    auto textureState = D3D12_RESOURCE_STATE_COPY_DEST;
    UploadStateTrace trace = {};
    upload_frame(core, texture.Get(), textureState, width, height, desc.input.inputFormat, frameIndex, nullptr, trace);
    encoder.write(texture.Get(), textureState);
    encoder.close();

    if (!std::filesystem::exists(outputPath) || std::filesystem::file_size(outputPath) == 0) {
        throw std::runtime_error("Handle plateau iteration produced an empty output.");
    }
}

void run_handle_plateau(const Options& options) {
    constexpr uint32_t warmupIterations = 5;
    constexpr uint32_t measuredIterations = 100;

    std::cout << "Handle plateau diagnostic\n";
    std::cout << "  warm-up initialization/shutdown iterations: " << warmupIterations << "\n";
    std::cout << "  measured initialization/shutdown iterations: " << measuredIterations << "\n";
    std::cout << "  per-iteration encode: 1 frame\n";
    std::cout << "  mode: " << options.mode << "\n";

    D3D12CoreLib::D3D12CoreConfig cfg;
    cfg.enableDebugLayer = options.debugLayer;
    cfg.enableInfoQueue = options.debugLayer;
    cfg.enableDred = true;
    cfg.allowWarpAdapter = false;
    auto core = D3D12CoreLib::D3D12Core::CreateShared(cfg);

    constexpr uint32_t width = 640;
    constexpr uint32_t height = 360;
    const auto cap = D3D12VideoEncoder::QueryD3D12VideoEncodeSupport(
        core.get(),
        VideoCodec::H264,
        VideoPixelFormat::NV12,
        width,
        height);
    if (!cap.supported) {
        throw std::runtime_error("Required native D3D12 H.264/NV12 640x360 capability is not supported.");
    }

    for (uint32_t i = 0; i < warmupIterations; ++i) {
        run_single_frame_encode_iteration(*core, options, options.output, i);
    }

    const DWORD baseline = current_handle_count();
    std::cout << "  handle baseline after warm-up: " << baseline << "\n";

    for (uint32_t i = 1; i <= measuredIterations; ++i) {
        run_single_frame_encode_iteration(*core, options, options.output, warmupIterations + i);
        if ((i % 10) == 0) {
            const DWORD count = current_handle_count();
            std::cout << "  iteration " << i
                      << " handleCount=" << count
                      << " deltaFromBaseline=" << (static_cast<long long>(count) - static_cast<long long>(baseline))
                      << "\n";
        }
    }

    const DWORD end = current_handle_count();
    std::cout << "  handle final: " << end
              << " deltaFromBaseline=" << (static_cast<long long>(end) - static_cast<long long>(baseline))
              << "\n";
}

Options parse_args(int argc, wchar_t** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto require_value = [&](const wchar_t* name) -> const wchar_t* {
            if (i + 1 >= argc) {
                throw std::runtime_error("Missing value for argument.");
            }
            (void)name;
            return argv[++i];
        };

        if (arg == L"--mode") {
            options.mode = narrow(require_value(L"--mode"));
        } else if (arg == L"--output") {
            options.output = require_value(L"--output");
        } else if (arg == L"--input") {
            options.input = require_value(L"--input");
        } else if (arg == L"--input-format") {
            options.inputFormat = narrow(require_value(L"--input-format"));
        } else if (arg == L"--frames") {
            options.frames = static_cast<uint32_t>(std::wcstoul(require_value(L"--frames"), nullptr, 10));
        } else if (arg == L"--gop-length") {
            options.gopLength = static_cast<uint32_t>(std::wcstoul(require_value(L"--gop-length"), nullptr, 10));
        } else if (arg == L"--diagnostic-timeout") {
            options.diagnosticTimeoutSeconds = static_cast<uint32_t>(std::wcstoul(require_value(L"--diagnostic-timeout"), nullptr, 10));
        } else if (arg == L"--async") {
            options.async = true;
        } else if (arg == L"--debug-layer") {
            options.debugLayer = true;
        } else if (arg == L"--handle-plateau") {
            options.handlePlateau = true;
        } else if (arg == L"--close-twice") {
            options.closeTwice = true;
        } else if (arg == L"--flush-before-close") {
            options.flushBeforeClose = true;
        } else if (arg == L"--stream-input") {
            options.streamInput = true;
        } else if (arg == L"--repeat-open-count") {
            options.repeatOpenCount = static_cast<uint32_t>(std::wcstoul(require_value(L"--repeat-open-count"), nullptr, 10));
        } else {
            throw std::runtime_error("Unknown argument: " + narrow(arg));
        }
    }
    if (options.output.empty()) {
        throw std::runtime_error("--output is required.");
    }
    if (options.frames == 0) {
        throw std::runtime_error("--frames must be non-zero.");
    }
    if (options.gopLength == 0) {
        throw std::runtime_error("--gop-length must be non-zero.");
    }
    if (options.repeatOpenCount == 0) {
        throw std::runtime_error("--repeat-open-count must be non-zero.");
    }
    if (options.mode != "direct" && options.mode != "rgba" && options.mode != "bgra") {
        throw std::runtime_error("--mode must be direct, rgba, or bgra.");
    }
    return options;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
#ifndef D3DVIDEOENCODER_TEST_HAS_D3D12_VIDEO_ENCODE
    std::cout << "D3D12 Video Encode backend is not enabled; skipping acceptance executable.\n";
    return 0;
#else
    try {
        const Options options = parse_args(argc, argv);
        constexpr uint32_t width = 640;
        constexpr uint32_t height = 360;
        constexpr uint32_t frameRateNum = 30;
        constexpr uint32_t frameRateDen = 1;
        if (options.handlePlateau) {
            run_handle_plateau(options);
            return 0;
        }
        const DXGI_FORMAT inputFormat = input_format_from_options(options);
        const bool rgbaInput = inputFormat == DXGI_FORMAT_R8G8B8A8_UNORM || inputFormat == DXGI_FORMAT_B8G8R8A8_UNORM;
        const auto rawInput = load_raw_input(options, inputFormat, width, height);

        D3D12CoreLib::D3D12CoreConfig cfg;
        cfg.enableDebugLayer = options.debugLayer;
        cfg.enableInfoQueue = options.debugLayer;
        cfg.enableDred = true;
        cfg.allowWarpAdapter = false;
        auto core = D3D12CoreLib::D3D12Core::CreateShared(cfg);

        const auto adapterName = narrow(core->DeviceContext().GetAdapterName());
        const LUID luid = core->GetAdapterLuid();
        std::cout << "Native D3D12 Video Encode acceptance\n";
        std::cout << "  adapter: " << adapterName << "\n";
        std::cout << "  LUID: " << luid_hex(luid) << "\n";
        std::cout << "  NodeIndex: 0\n";
        std::cout << "  codec: H.264\n";
        std::cout << "  profile: High\n";
        std::cout << "  input format: " << input_format_name(inputFormat) << "\n";
        std::cout << "  input source: " << (options.input.empty() ? "synthetic" : options.input.string()) << "\n";
        std::cout << "  stream input: " << (rawInput.streaming ? "true" : "false") << "\n";
        std::cout << "  internal format: NV12\n";
        std::cout << "  resolution: " << width << "x" << height << "\n";
        std::cout << "  frame rate: " << frameRateNum << "/" << frameRateDen << "\n";
        std::cout << "  rate control: CBR-capable path when supported by query\n";
        std::cout << "  frames: " << options.frames << "\n";
        std::cout << "  gopLength: " << options.gopLength << "\n";
        std::cout << "  async: " << (options.async ? "true" : "false") << "\n";
        std::cout << "  debug layer requested: " << (options.debugLayer ? "true" : "false") << "\n";
        std::cout << "  DRED forced in acceptance executable: true\n";
        std::cout << "  diagnostic timeout seconds: " << options.diagnosticTimeoutSeconds << "\n";
        print_adapter_identity(core->GetDevice(), adapterName);

        const auto cap = D3D12VideoEncoder::QueryD3D12VideoEncodeSupport(
            core.get(),
            VideoCodec::H264,
            VideoPixelFormat::NV12,
            width,
            height);
        std::cout << "  full support/capability result: supported=" << cap.supported
                  << " hr=" << hr_hex(cap.queryHr)
                  << " cbr=" << cap.cbrSupported
                  << " cqp=" << cap.cqpSupported
                  << " message=" << cap.message << "\n";
        if (!cap.supported) {
            throw std::runtime_error("Required native D3D12 H.264/NV12 640x360 capability is not supported.");
        }

        std::filesystem::create_directories(options.output.parent_path());

        D3D12VideoEncoderDesc desc;
        desc.width = width;
        desc.height = height;
        desc.frameRateNum = frameRateNum;
        desc.frameRateDen = frameRateDen;
        desc.backend = D3DVideoEncoderBackendType::D3D12VideoEncode;
        desc.codec = VideoCodec::H264;
        desc.internalFormat = VideoPixelFormat::NV12;
        desc.bitrate = 4'000'000;
        desc.gopLength = options.gopLength;
        desc.bFrameCount = 0;
        desc.rateControl = VideoRateControlMode::CBR;
        desc.asyncMode = options.async;
        desc.queueDepth = 2;
        desc.queueFullPolicy = EncoderQueueFullPolicy::Block;
        desc.enableDebugLog = true;
        desc.input.core = core.get();
        desc.input.inputFormat = inputFormat;
        desc.input.allowFormatConversion = rgbaInput;
        desc.input.processingShaderDirectory = D3DVIDEOENCODER_TEST_D3D12_SHADER_DIR;
        desc.input.restoreStateAfterEncode = true;

        std::cout << "  first frame: uploading and encoding\n";
        std::cout << "  closeTwice: " << (options.closeTwice ? "true" : "false") << "\n";
        std::cout << "  flushBeforeClose: " << (options.flushBeforeClose ? "true" : "false") << "\n";
        std::cout << "  repeatOpenCount: " << options.repeatOpenCount << "\n";
        std::cout << "  process working set start: " << current_working_set_bytes() << "\n";
        std::cout << "  handle count start: " << current_handle_count() << "\n";
        D3D12CoreLib::D3D12Core& coreRef = *core;
        std::atomic<uint32_t> currentFrame = 0;
        std::atomic<unsigned long> lastBeforeUpload = static_cast<unsigned long>(D3D12_RESOURCE_STATE_COPY_DEST);
        std::atomic<unsigned long> lastAfterCopy = static_cast<unsigned long>(D3D12_RESOURCE_STATE_COPY_DEST);
        std::atomic<unsigned long> lastAfterHandoff = static_cast<unsigned long>(D3D12_RESOURCE_STATE_COPY_DEST);
        std::atomic<unsigned long> lastStatePassedToWrite = static_cast<unsigned long>(D3D12_RESOURCE_STATE_COMMON);
        std::filesystem::path finalOutput = options.output;

        auto output_for_iteration = [&](uint32_t iteration) {
            if (options.repeatOpenCount <= 1) {
                return options.output;
            }
            const auto stem = options.output.stem().wstring() + L"_iter" + std::to_wstring(iteration);
            return options.output.parent_path() / (stem + options.output.extension().wstring());
        };

        auto runEncodeOnce = [&](uint32_t iteration) {
            const auto iterationOutput = output_for_iteration(iteration);
            finalOutput = iterationOutput;
            std::filesystem::remove(iterationOutput);
            desc.outputPath = iterationOutput.wstring();
            D3D12VideoEncoder encoder(desc);
            std::ifstream streamedInput;
            std::vector<uint8_t> streamedFrame;
            if (rawInput.streaming) {
                streamedInput.open(rawInput.path, std::ios::binary);
                if (!streamedInput) {
                    throw std::runtime_error("Failed to open streaming raw input: " + rawInput.path.string());
                }
                streamedFrame.resize(rawInput.frameSize);
            }
            auto raw_frame_for = [&](uint32_t frameIndex) -> const uint8_t* {
                if (!rawInput.bytes.empty()) {
                    return rawInput.bytes.data() + static_cast<size_t>(frameIndex) * rawInput.frameSize;
                }
                if (!rawInput.streaming) {
                    return nullptr;
                }
                streamedInput.read(reinterpret_cast<char*>(streamedFrame.data()), static_cast<std::streamsize>(streamedFrame.size()));
                if (streamedInput.gcount() != static_cast<std::streamsize>(streamedFrame.size())) {
                    throw std::runtime_error("Failed to read streaming raw frame " + std::to_string(frameIndex));
                }
                return streamedFrame.data();
            };
            if (options.async) {
                std::vector<ComPtr<ID3D12Resource>> textures;
                textures.reserve(options.frames);
                for (uint32_t i = 0; i < options.frames; ++i) {
                    currentFrame.store(i);
                    auto texture = create_texture(coreRef, width, height, desc.input.inputFormat);
                    auto textureState = D3D12_RESOURCE_STATE_COPY_DEST;
                    const uint8_t* rawFrame = raw_frame_for(i);
                    UploadStateTrace lastTrace = {};
                    upload_frame(coreRef, texture.Get(), textureState, width, height, desc.input.inputFormat, i, rawFrame, lastTrace);
                    lastBeforeUpload.store(static_cast<unsigned long>(lastTrace.beforeUpload));
                    lastAfterCopy.store(static_cast<unsigned long>(lastTrace.afterCopy));
                    lastAfterHandoff.store(static_cast<unsigned long>(lastTrace.afterHandoff));
                    lastStatePassedToWrite.store(static_cast<unsigned long>(textureState));
                    encoder.write(texture.Get(), textureState);
                    textures.push_back(std::move(texture));
                }
            } else {
                auto texture = create_texture(coreRef, width, height, desc.input.inputFormat);
                auto textureState = D3D12_RESOURCE_STATE_COPY_DEST;
                for (uint32_t i = 0; i < options.frames; ++i) {
                    currentFrame.store(i);
                    const uint8_t* rawFrame = raw_frame_for(i);
                    UploadStateTrace lastTrace = {};
                    upload_frame(coreRef, texture.Get(), textureState, width, height, desc.input.inputFormat, i, rawFrame, lastTrace);
                    lastBeforeUpload.store(static_cast<unsigned long>(lastTrace.beforeUpload));
                    lastAfterCopy.store(static_cast<unsigned long>(lastTrace.afterCopy));
                    lastAfterHandoff.store(static_cast<unsigned long>(lastTrace.afterHandoff));
                    lastStatePassedToWrite.store(static_cast<unsigned long>(textureState));
                    encoder.write(texture.Get(), textureState);
                    textureState = static_cast<D3D12_RESOURCE_STATES>(lastStatePassedToWrite.load());
                }
            }
            if (options.flushBeforeClose) {
                encoder.flush();
                std::cout << "  iteration " << iteration << " flush result: success\n";
            }
            encoder.close();
            std::cout << "  iteration " << iteration << " close result: success\n";
            if (options.closeTwice) {
                encoder.close();
                std::cout << "  iteration " << iteration << " second close result: success\n";
            }
        };

        auto runEncode = [&]() {
            for (uint32_t iteration = 0; iteration < options.repeatOpenCount; ++iteration) {
                runEncodeOnce(iteration);
            }
        };

        auto print_failure_snapshot = [&]() {
            std::cerr << "  failure frame index: " << currentFrame.load() << "\n";
            std::cerr << "  resource state before upload: "
                      << resource_state_name(static_cast<D3D12_RESOURCE_STATES>(lastBeforeUpload.load())) << "\n";
            std::cerr << "  resource state after upload copy: "
                      << resource_state_name(static_cast<D3D12_RESOURCE_STATES>(lastAfterCopy.load())) << "\n";
            std::cerr << "  resource state after COMMON transition: "
                      << resource_state_name(static_cast<D3D12_RESOURCE_STATES>(lastAfterHandoff.load())) << "\n";
            std::cerr << "  resource state passed to encoder.write: "
                      << resource_state_name(static_cast<D3D12_RESOURCE_STATES>(lastStatePassedToWrite.load())) << "\n";
            std::cerr << "  input/output format: " << input_format_name(inputFormat) << " -> NV12\n";
            print_info_queue(coreRef.GetDevice());
            print_device_removed_and_dred(coreRef.GetDevice());
        };

        try {
            if (options.diagnosticTimeoutSeconds == 0) {
                runEncode();
            } else {
                std::atomic<bool> done = false;
                std::exception_ptr workerException;
                std::thread worker([&]() {
                    try {
                        runEncode();
                    } catch (...) {
                        workerException = std::current_exception();
                    }
                    done.store(true);
                });
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.diagnosticTimeoutSeconds);
                while (!done.load() && std::chrono::steady_clock::now() < deadline) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (!done.load()) {
                    std::cerr << "  diagnostic timeout expired after " << options.diagnosticTimeoutSeconds << " seconds\n";
                    print_failure_snapshot();
                    std::cout.flush();
                    std::cerr.flush();
                    std::quick_exit(124);
                }
                worker.join();
                if (workerException) {
                    std::rethrow_exception(workerException);
                }
            }
        } catch (...) {
            print_failure_snapshot();
            throw;
        }

        const auto fileSize = std::filesystem::file_size(finalOutput);
        if (options.debugLayer) {
            print_info_queue(coreRef.GetDevice());
        }
        std::cout << "  final summary: output=" << finalOutput.string()
                  << " size=" << fileSize << " bytes\n";
        std::cout << "  process working set end: " << current_working_set_bytes() << "\n";
        std::cout << "  handle count end: " << current_handle_count() << "\n";
        if (fileSize == 0) {
            throw std::runtime_error("Encoded output is empty.");
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Native D3D12 Video Encode acceptance failed: " << e.what() << "\n";
        return 1;
    }
#endif
}
