#include <MFFrameSource/MFD3D12CameraCaptureThread.hpp>
#include <MFFrameSource/MFD3D12CameraSyncThread.hpp>
#include <MFFrameSource/MFPlatformContext.hpp>

#include <D3D12Helper/D3D12Core/D3D12Core.hpp>

#include <Windows.h>
#include <mfapi.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace MFFrameSource;
using Microsoft::WRL::ComPtr;

namespace {

void PrintUsage() {
    std::wcout << L"usage:\n"
               << L"  d3d12_stereo_sync <leftIndex> <rightIndex> <width> <height> <fpsNum> <fpsDen> <subtype> <shaderDir> [pairCount] [outputMp4|-] [bitrate]\n"
               << L"subtype: NV12 | P010 | RGB32 | ARGB32\n"
               << L"outputMp4: default=stereo_sync_output.mp4, '-' disables video writing\n"
               << L"bitrate:   default=50000000\n";
}

void ThrowIfFailed(HRESULT hr, const char* where) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(where) + " failed, hr=0x" + std::to_string(static_cast<unsigned long>(hr)));
    }
}

GUID ParseSubtype(const std::wstring& s) {
    if (s == L"NV12") return MFVideoFormat_NV12;
    if (s == L"P010") return MFVideoFormat_P010;
    if (s == L"RGB32") return MFVideoFormat_RGB32;
    if (s == L"ARGB32") return MFVideoFormat_ARGB32;
    throw std::runtime_error("unknown subtype");
}

MFD3D12CameraCaptureThreadConfig MakeConfig(int deviceIndex, int argc, wchar_t** argv) {
    MFD3D12CameraCaptureThreadConfig cfg;
    cfg.selector.deviceIndex = deviceIndex;
    cfg.capture.input.width = static_cast<UINT>(std::stoul(argv[3]));
    cfg.capture.input.height = static_cast<UINT>(std::stoul(argv[4]));
    cfg.capture.input.fps.numerator = static_cast<std::uint32_t>(std::stoul(argv[5]));
    cfg.capture.input.fps.denominator = static_cast<std::uint32_t>(std::stoul(argv[6]));
    cfg.capture.input.subtype = ParseSubtype(argv[7]);
    cfg.capture.processingShaderDirectory = argv[8];
    cfg.capture.outputWidth = cfg.capture.input.width;
    cfg.capture.outputHeight = cfg.capture.input.height;
    cfg.capture.framePoolSize = 4;
    cfg.defaultQueueCapacity = 4;
    cfg.clonePoolSize = 8;
    (void)argc;
    return cfg;
}

class StereoPreviewWindow {
public:
    StereoPreviewWindow(UINT imageWidth, UINT imageHeight) {
        hinstance_ = GetModuleHandleW(nullptr);
        const wchar_t* className = L"MFFrameSourceStereoPreviewWindow";

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &StereoPreviewWindow::WndProc;
        wc.hInstance = hinstance_;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = className;
        RegisterClassExW(&wc);

        const UINT maxClientW = 1600;
        const UINT maxClientH = 900;
        double scale = 1.0;
        if (imageWidth > maxClientW || imageHeight > maxClientH) {
            const double sx = static_cast<double>(maxClientW) / static_cast<double>(imageWidth);
            const double sy = static_cast<double>(maxClientH) / static_cast<double>(imageHeight);
            scale = std::min(sx, sy);
        }
        const int clientW = std::max(1, static_cast<int>(imageWidth * scale));
        const int clientH = std::max(1, static_cast<int>(imageHeight * scale));

        RECT rc{0, 0, clientW, clientH};
        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);

        hwnd_ = CreateWindowExW(
            0,
            className,
            L"MFFrameSource stereo preview",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            rc.right - rc.left,
            rc.bottom - rc.top,
            nullptr,
            nullptr,
            hinstance_,
            this);
        if (!hwnd_) {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()), "CreateWindowExW");
        }
    }

    ~StereoPreviewWindow() {
        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
    }

    StereoPreviewWindow(const StereoPreviewWindow&) = delete;
    StereoPreviewWindow& operator=(const StereoPreviewWindow&) = delete;

    bool processMessages() {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                closed_ = true;
                return false;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return !closed_;
    }

    bool present(const std::vector<std::uint8_t>& bgra, UINT width, UINT height) {
        if (!hwnd_ || closed_) return false;
        image_ = bgra;
        imageWidth_ = width;
        imageHeight_ = height;
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
        return processMessages();
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        StereoPreviewWindow* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            self = static_cast<StereoPreviewWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        } else {
            self = reinterpret_cast<StereoPreviewWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (self) {
            return self->handleMessage(hwnd, msg, wp, lp);
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT handleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CLOSE:
            closed_ = true;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            closed_ = true;
            if (hwnd_ == hwnd) hwnd_ = nullptr;
            PostQuitMessage(0);
            return 0;
        case WM_PAINT:
            paint(hwnd);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    void paint(HWND hwnd) {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        if (!image_.empty() && imageWidth_ != 0 && imageHeight_ != 0) {
            RECT rc{};
            GetClientRect(hwnd, &rc);

            // The CPU canvas is explicitly BGRA8.  Some GDI paths/drivers render
            // 32-bit BI_RGB DIBs ambiguously, and the default stretch mode can
            // create strong colour aliasing when downscaling a 3840x1080 stereo
            // image into a smaller preview window.  Use explicit BGRA bit masks
            // and HALFTONE scaling for the preview only.  The MP4 writer still
            // receives the same BGRA/RGB32 memory as before.
            BITMAPV5HEADER bmi{};
            bmi.bV5Size = sizeof(BITMAPV5HEADER);
            bmi.bV5Width = static_cast<LONG>(imageWidth_);
            bmi.bV5Height = -static_cast<LONG>(imageHeight_); // top-down
            bmi.bV5Planes = 1;
            bmi.bV5BitCount = 32;
            bmi.bV5Compression = BI_BITFIELDS;
            bmi.bV5RedMask = 0x00FF0000;
            bmi.bV5GreenMask = 0x0000FF00;
            bmi.bV5BlueMask = 0x000000FF;
            bmi.bV5AlphaMask = 0xFF000000;

            const int oldStretchMode = SetStretchBltMode(hdc, HALFTONE);
            POINT oldBrushOrg{};
            SetBrushOrgEx(hdc, 0, 0, &oldBrushOrg);

            StretchDIBits(
                hdc,
                0,
                0,
                rc.right - rc.left,
                rc.bottom - rc.top,
                0,
                0,
                static_cast<int>(imageWidth_),
                static_cast<int>(imageHeight_),
                image_.data(),
                reinterpret_cast<const BITMAPINFO*>(&bmi),
                DIB_RGB_COLORS,
                SRCCOPY);

            SetBrushOrgEx(hdc, oldBrushOrg.x, oldBrushOrg.y, nullptr);
            if (oldStretchMode != 0) {
                SetStretchBltMode(hdc, oldStretchMode);
            }
        }
        EndPaint(hwnd, &ps);
    }

    HINSTANCE hinstance_ = nullptr;
    HWND hwnd_ = nullptr;
    bool closed_ = false;
    UINT imageWidth_ = 0;
    UINT imageHeight_ = 0;
    std::vector<std::uint8_t> image_;
};

class StereoReadbackToBgra {
public:
    void initialize(std::shared_ptr<D3D12CoreLib::D3D12Core> core, UINT singleWidth, UINT height) {
        if (!core) throw std::invalid_argument("StereoReadbackToBgra::initialize: null D3D12Core");
        core_ = std::move(core);
        singleWidth_ = singleWidth;
        height_ = height;
        context_ = core_->CreateDirectContext();
        createReadback(left_);
        createReadback(right_);
    }

    void readPair(MFD3D12CameraFrame& leftFrame,
                  MFD3D12CameraFrame& rightFrame,
                  std::vector<std::uint8_t>& outBgra) {
        leftFrame.waitReady();
        rightFrame.waitReady();

        if (leftFrame.width() != singleWidth_ || rightFrame.width() != singleWidth_ ||
            leftFrame.height() != height_ || rightFrame.height() != height_) {
            throw std::runtime_error("StereoReadbackToBgra: frame size changed");
        }
        if (!isSupportedOutputFormat(leftFrame.format()) || !isSupportedOutputFormat(rightFrame.format())) {
            throw std::runtime_error("StereoReadbackToBgra: only RGBA8/BGRA8 output textures can be previewed/written");
        }

        auto& leftResource = leftFrame.resource();
        auto& rightResource = rightFrame.resource();
        const auto leftBefore = leftResource.GetState();
        const auto rightBefore = rightResource.GetState();

        context_.Reset();
        transitionIfNeeded(leftResource, leftBefore, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transitionIfNeeded(rightResource, rightBefore, D3D12_RESOURCE_STATE_COPY_SOURCE);
        recordCopy(left_, leftResource.Get());
        recordCopy(right_, rightResource.Get());
        transitionIfNeeded(leftResource, D3D12_RESOURCE_STATE_COPY_SOURCE, leftBefore);
        transitionIfNeeded(rightResource, D3D12_RESOURCE_STATE_COPY_SOURCE, rightBefore);
        context_.Close();

        ID3D12CommandList* lists[] = { context_.GetCommandList() };
        core_->DirectQueue().ExecuteCommandLists(1, lists);
        const UINT64 fenceValue = core_->DirectQueue().Signal();
        core_->DirectQueue().WaitForFenceValue(fenceValue);

        outBgra.resize(static_cast<std::size_t>(singleWidth_) * 2u * height_ * 4u);
        copyMappedToCanvas(left_, leftFrame.format(), outBgra, 0);
        copyMappedToCanvas(right_, rightFrame.format(), outBgra, singleWidth_);
    }

private:
    struct ReadbackSlot {
        ComPtr<ID3D12Resource> buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
        UINT numRows = 0;
        UINT64 rowSizeBytes = 0;
        UINT64 totalBytes = 0;
    };

    static bool isSupportedOutputFormat(DXGI_FORMAT fmt) noexcept {
        return fmt == DXGI_FORMAT_R8G8B8A8_UNORM ||
               fmt == DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    void createReadback(ReadbackSlot& slot) {
        D3D12_RESOURCE_DESC texDesc{};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Alignment = 0;
        texDesc.Width = singleWidth_;
        texDesc.Height = height_;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;
        texDesc.SampleDesc.Quality = 0;
        texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        core_->GetDevice()->GetCopyableFootprints(
            &texDesc,
            0,
            1,
            0,
            &slot.layout,
            &slot.numRows,
            &slot.rowSizeBytes,
            &slot.totalBytes);

        D3D12_HEAP_PROPERTIES heapProps{};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProps.CreationNodeMask = 1;
        heapProps.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC bufferDesc{};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Alignment = 0;
        bufferDesc.Width = slot.totalBytes;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.SampleDesc.Quality = 0;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

        ThrowIfFailed(core_->GetDevice()->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&slot.buffer)),
            "CreateCommittedResource(readback)");
    }

    void transitionIfNeeded(D3D12CoreLib::D3D12Resource& resource,
                            D3D12_RESOURCE_STATES before,
                            D3D12_RESOURCE_STATES after) {
        if (before == after) return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource = resource.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        context_.ResourceBarrier(barrier);
        resource.SetState(after);
    }

    void recordCopy(const ReadbackSlot& slot, ID3D12Resource* src) {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = slot.buffer.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = slot.layout;

        D3D12_TEXTURE_COPY_LOCATION srcLoc{};
        srcLoc.pResource = src;
        srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        srcLoc.SubresourceIndex = 0;

        context_.GetCommandList()->CopyTextureRegion(&dst, 0, 0, 0, &srcLoc, nullptr);
    }

    void copyMappedToCanvas(const ReadbackSlot& slot,
                            DXGI_FORMAT sourceFormat,
                            std::vector<std::uint8_t>& canvasBgra,
                            UINT dstXOffset) {
        void* mapped = nullptr;
        D3D12_RANGE readRange{0, static_cast<SIZE_T>(slot.totalBytes)};
        ThrowIfFailed(slot.buffer->Map(0, &readRange, &mapped), "Map(readback)");

        const auto* base = static_cast<const std::uint8_t*>(mapped) + slot.layout.Offset;
        const UINT canvasWidth = singleWidth_ * 2;
        for (UINT y = 0; y < height_; ++y) {
            const auto* srcRow = base + static_cast<std::size_t>(y) * slot.layout.Footprint.RowPitch;
            auto* dstRow = canvasBgra.data() +
                (static_cast<std::size_t>(y) * canvasWidth + dstXOffset) * 4u;

            for (UINT x = 0; x < singleWidth_; ++x) {
                const auto* s = srcRow + static_cast<std::size_t>(x) * 4u;
                auto* d = dstRow + static_cast<std::size_t>(x) * 4u;
                if (sourceFormat == DXGI_FORMAT_R8G8B8A8_UNORM) {
                    d[0] = s[2];
                    d[1] = s[1];
                    d[2] = s[0];
                    d[3] = 255;
                } else {
                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                    d[3] = 255;
                }
            }
        }

        D3D12_RANGE writtenRange{0, 0};
        slot.buffer->Unmap(0, &writtenRange);
    }

    std::shared_ptr<D3D12CoreLib::D3D12Core> core_;
    D3D12CoreLib::D3D12CommandContext context_;
    UINT singleWidth_ = 0;
    UINT height_ = 0;
    ReadbackSlot left_;
    ReadbackSlot right_;
};

class MfRgb32H264Writer {
public:
    ~MfRgb32H264Writer() noexcept {
        try { close(); } catch (...) {}
    }

    MfRgb32H264Writer(const MfRgb32H264Writer&) = delete;
    MfRgb32H264Writer& operator=(const MfRgb32H264Writer&) = delete;
    MfRgb32H264Writer() = default;

    void open(const std::wstring& path,
              UINT width,
              UINT height,
              std::uint32_t fpsNum,
              std::uint32_t fpsDen,
              UINT32 bitrate) {
        if (path.empty() || path == L"-") {
            enabled_ = false;
            return;
        }
        if (fpsNum == 0 || fpsDen == 0) {
            throw std::invalid_argument("MfRgb32H264Writer::open: invalid frame rate");
        }

        width_ = width;
        height_ = height;
        frameDuration100ns_ = static_cast<LONGLONG>((10000000ull * fpsDen) / fpsNum);
        frameIndex_ = 0;

        ThrowIfFailed(MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &writer_),
                      "MFCreateSinkWriterFromURL");

        ComPtr<IMFMediaType> outputType;
        ThrowIfFailed(MFCreateMediaType(&outputType), "MFCreateMediaType(output)");
        ThrowIfFailed(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set output major type");
        ThrowIfFailed(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "Set output subtype");
        ThrowIfFailed(outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrate), "Set output bitrate");
        ThrowIfFailed(outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Set output interlace");
        ThrowIfFailed(MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width, height), "Set output frame size");
        ThrowIfFailed(MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, fpsNum, fpsDen), "Set output frame rate");
        ThrowIfFailed(MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Set output PAR");
        ThrowIfFailed(writer_->AddStream(outputType.Get(), &streamIndex_), "AddStream");

        ComPtr<IMFMediaType> inputType;
        ThrowIfFailed(MFCreateMediaType(&inputType), "MFCreateMediaType(input)");
        ThrowIfFailed(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Set input major type");
        ThrowIfFailed(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), "Set input subtype");
        ThrowIfFailed(inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Set input interlace");
        ThrowIfFailed(MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, width, height), "Set input frame size");
        ThrowIfFailed(MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, fpsNum, fpsDen), "Set input frame rate");
        ThrowIfFailed(MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Set input PAR");
        ThrowIfFailed(inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, width * 4), "Set input stride");
        ThrowIfFailed(writer_->SetInputMediaType(streamIndex_, inputType.Get(), nullptr), "SetInputMediaType");
        ThrowIfFailed(writer_->BeginWriting(), "BeginWriting");
        enabled_ = true;
    }

    bool enabled() const noexcept { return enabled_; }

    void writeFrame(const std::vector<std::uint8_t>& bgra) {
        if (!enabled_) return;
        const DWORD bytes = static_cast<DWORD>(bgra.size());
        if (bytes != width_ * height_ * 4u) {
            throw std::runtime_error("MfRgb32H264Writer::writeFrame: unexpected frame size");
        }

        ComPtr<IMFMediaBuffer> buffer;
        ThrowIfFailed(MFCreateMemoryBuffer(bytes, &buffer), "MFCreateMemoryBuffer");

        BYTE* dst = nullptr;
        DWORD maxLen = 0;
        DWORD currentLen = 0;
        ThrowIfFailed(buffer->Lock(&dst, &maxLen, &currentLen), "IMFMediaBuffer::Lock");
        std::copy(bgra.begin(), bgra.end(), dst);
        ThrowIfFailed(buffer->Unlock(), "IMFMediaBuffer::Unlock");
        ThrowIfFailed(buffer->SetCurrentLength(bytes), "IMFMediaBuffer::SetCurrentLength");

        ComPtr<IMFSample> sample;
        ThrowIfFailed(MFCreateSample(&sample), "MFCreateSample");
        ThrowIfFailed(sample->AddBuffer(buffer.Get()), "IMFSample::AddBuffer");
        ThrowIfFailed(sample->SetSampleTime(frameIndex_ * frameDuration100ns_), "IMFSample::SetSampleTime");
        ThrowIfFailed(sample->SetSampleDuration(frameDuration100ns_), "IMFSample::SetSampleDuration");
        ThrowIfFailed(writer_->WriteSample(streamIndex_, sample.Get()), "IMFSinkWriter::WriteSample");
        ++frameIndex_;
    }

    void close() {
        if (writer_) {
            ThrowIfFailed(writer_->Finalize(), "IMFSinkWriter::Finalize");
            writer_.Reset();
        }
        enabled_ = false;
    }

private:
    ComPtr<IMFSinkWriter> writer_;
    DWORD streamIndex_ = 0;
    UINT width_ = 0;
    UINT height_ = 0;
    LONGLONG frameDuration100ns_ = 0;
    LONGLONG frameIndex_ = 0;
    bool enabled_ = false;
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 9) {
            PrintUsage();
            return 2;
        }
        const std::uint32_t pairCount = argc >= 10 ? static_cast<std::uint32_t>(std::stoul(argv[9])) : 120u;
        const std::wstring outputPath = argc >= 11 ? argv[10] : L"stereo_sync_output.mp4";
        const UINT32 bitrate = argc >= 12 ? static_cast<UINT32>(std::stoul(argv[11])) : 50000000u;

        MFPlatformContext platform;

        D3D12CoreLib::D3D12CoreConfig d3dCfg;
        d3dCfg.enableDebugLayer = true;
        d3dCfg.allowWarpAdapter = false;
        auto core = D3D12CoreLib::D3D12Core::CreateShared(d3dCfg);

        MFD3D12CameraCaptureThread left;
        MFD3D12CameraCaptureThread right;
        auto leftCfg = MakeConfig(std::stoi(argv[1]), argc, argv);
        auto rightCfg = MakeConfig(std::stoi(argv[2]), argc, argv);

        if (!left.open(leftCfg, core)) {
            const auto& e = left.lastError();
            std::wcerr << L"left open failed: " << e.where << L": " << e.message << L"\n";
            return 1;
        }
        if (!right.open(rightCfg, core)) {
            const auto& e = right.lastError();
            std::wcerr << L"right open failed: " << e.where << L": " << e.message << L"\n";
            return 1;
        }

        auto lq = left.createQueue(4);
        auto rq = right.createQueue(4);

        MFD3D12CameraSyncThread sync;
        MFD3D12CameraSyncThreadConfig syncCfg;
        syncCfg.maxAdjustedDiff100ns = 50000;
        syncCfg.candidateCapacity = 16;
        syncCfg.outputQueueCapacity = 4;
        if (!sync.open(lq, rq, syncCfg)) {
            std::wcerr << L"sync open failed\n";
            return 1;
        }

        const UINT singleWidth = left.selectedFormat().width;
        const UINT singleHeight = left.selectedFormat().height;
        const UINT canvasWidth = singleWidth * 2;
        const UINT canvasHeight = singleHeight;
        const auto fpsNum = left.selectedFormat().fps.numerator;
        const auto fpsDen = left.selectedFormat().fps.denominator;

        StereoPreviewWindow preview(canvasWidth, canvasHeight);
        StereoReadbackToBgra readback;
        readback.initialize(core, singleWidth, singleHeight);

        MfRgb32H264Writer writer;
        writer.open(outputPath, canvasWidth, canvasHeight, fpsNum, fpsDen, bitrate);
        if (writer.enabled()) {
            std::wcout << L"writing stereo video: " << outputPath << L"\n";
        } else {
            std::wcout << L"video writing disabled\n";
        }

        left.start();
        right.start();
        sync.start();

        auto out = sync.outputQueue();
        std::vector<std::uint8_t> canvasBgra;
        std::uint32_t received = 0;
        while (received < pairCount) {
            if (!preview.processMessages()) {
                std::wcout << L"preview window closed\n";
                break;
            }

            auto pair = out->waitPopFor(std::chrono::seconds(5));
            if (!pair) {
                std::wcerr << L"timeout waiting for stereo pair\n";
                break;
            }

            readback.readPair(pair->left, pair->right, canvasBgra);
            writer.writeFrame(canvasBgra);
            if (!preview.present(canvasBgra, canvasWidth, canvasHeight)) {
                std::wcout << L"preview window closed\n";
                break;
            }

            std::wcout << L"pair " << pair->pairNumber
                       << L" diff100ns=" << pair->timestampDiff100ns
                       << L" adjusted100ns=" << pair->adjustedDiff100ns
                       << L" baseline100ns=" << pair->baselineDiff100ns << L"\n";
            ++received;
        }

        writer.close();
        sync.stop();
        left.stop();
        right.stop();

        auto ss = sync.stats();
        std::wcout << L"sync stats pairs=" << ss.pairsOut
                   << L" leftIn=" << ss.leftFramesIn
                   << L" rightIn=" << ss.rightFramesIn
                   << L" dropL=" << ss.droppedLeft
                   << L" dropR=" << ss.droppedRight << L"\n";
        return received == 0 ? 1 : 0;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}
