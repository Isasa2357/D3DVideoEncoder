#include "backend/d3d12video/D3D12VideoEncodeDiagnostic.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <d3d12video.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <exception>
#include <iostream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace {

void throw_if_failed(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        throw std::runtime_error(std::string(what) + " failed. HRESULT=" + D3DVideoEncoderLib::Diagnostics::HrHex(hr));
    }
}

} // namespace

int main() {
    try {
        ComPtr<IDXGIFactory6> factory;
        throw_if_failed(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");

        ComPtr<IDXGIAdapter1> selectedAdapter;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT hr = factory->EnumAdapterByGpuPreference(
                index,
                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&candidate));
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            throw_if_failed(hr, "EnumAdapterByGpuPreference");
            DXGI_ADAPTER_DESC1 candidateDesc = {};
            throw_if_failed(candidate->GetDesc1(&candidateDesc), "IDXGIAdapter1::GetDesc1(candidate)");
            if ((candidateDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr))) {
                selectedAdapter = candidate;
                break;
            }
        }
        if (!selectedAdapter) {
            throw std::runtime_error("No hardware D3D12 adapter was found.");
        }

        ComPtr<ID3D12Device> device;
        throw_if_failed(D3D12CreateDevice(selectedAdapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "D3D12CreateDevice");
        const LUID currentLuid = device->GetAdapterLuid();

        ComPtr<IDXGIAdapter1> adapter;
        throw_if_failed(factory->EnumAdapterByLuid(currentLuid, IID_PPV_ARGS(&adapter)), "EnumAdapterByLuid(current device LUID)");

        DXGI_ADAPTER_DESC1 adapterDesc = {};
        throw_if_failed(adapter->GetDesc1(&adapterDesc), "IDXGIAdapter1::GetDesc1");

        ComPtr<ID3D12VideoDevice> videoDevice;
        const HRESULT videoDeviceHr = device.As(&videoDevice);

        ComPtr<ID3D12VideoDevice3> videoDevice3;
        const HRESULT videoDevice3Hr = device.As(&videoDevice3);

        std::cout << "Native D3D12 Video Encode minimal probe\n";
        std::cout << "  executable absolute path: " << D3DVideoEncoderLib::Diagnostics::ModulePathUtf8(nullptr) << "\n";
        std::cout << "  D3DVideoEncoder library: static library linked into executable\n";
        std::cout << "  D3D12Helper library/module: static library linked into executable\n";
#ifdef NDEBUG
        std::cout << "  build configuration: Release (NDEBUG defined)\n";
#else
        std::cout << "  build configuration: Debug (NDEBUG not defined)\n";
#endif
        std::cout << "  Windows SDK target version: " << D3DVE_DIAGNOSTIC_WINDOWS_SDK_VERSION << "\n";
        std::cout << "  adapter name: " << D3DVideoEncoderLib::Diagnostics::Narrow(adapterDesc.Description) << "\n";
        std::cout << "  adapter LUID: " << D3DVideoEncoderLib::Diagnostics::LuidHex(adapterDesc.AdapterLuid) << "\n";
        std::cout << "  adapter vendor ID: " << D3DVideoEncoderLib::Diagnostics::Hex32(adapterDesc.VendorId)
                  << " device ID: " << D3DVideoEncoderLib::Diagnostics::Hex32(adapterDesc.DeviceId) << "\n";
        std::cout << "  expected adapter: NVIDIA GeForce RTX 5070 Ti\n";
        std::cout << "  LUID policy: current ID3D12Device::GetAdapterLuid, verified by EnumAdapterByLuid\n";
        std::cout << "  NodeIndex: 0\n";
        std::cout << "  ID3D12VideoDevice: " << (SUCCEEDED(videoDeviceHr) && videoDevice ? "yes" : "no")
                  << " hr=" << D3DVideoEncoderLib::Diagnostics::HrHex(videoDeviceHr) << "\n";
        std::cout << "  ID3D12VideoDevice3: " << (SUCCEEDED(videoDevice3Hr) && videoDevice3 ? "yes" : "no")
                  << " hr=" << D3DVideoEncoderLib::Diagnostics::HrHex(videoDevice3Hr) << "\n";

        D3DVideoEncoderLib::Diagnostics::NativeD3D12H264Nv12DiagnosticResult result;
        D3DVideoEncoderLib::Diagnostics::PrintLightweightProfileLevelProbe(videoDevice.Get(), std::cout, result);

        D3D12_VIDEO_ENCODER_VALIDATION_FLAGS validationFlags =
            static_cast<D3D12_VIDEO_ENCODER_VALIDATION_FLAGS>(0);
        D3D12_VIDEO_ENCODER_SUPPORT_FLAGS supportFlags =
            static_cast<D3D12_VIDEO_ENCODER_SUPPORT_FLAGS>(0);
        const HRESULT unbufferedHr = D3DVideoEncoderLib::Diagnostics::RunFullSupportQuery(
            videoDevice.Get(),
            false,
            std::cout,
            validationFlags,
            supportFlags);
        const HRESULT bufferedHr = D3DVideoEncoderLib::Diagnostics::RunFullSupportQuery(
            videoDevice.Get(),
            true,
            std::cout,
            validationFlags,
            supportFlags);

        std::cout << "Minimal probe summary\n";
        std::cout << "  lightweight: supported=" << result.profileLevelIsSupported
                  << " hr=" << D3DVideoEncoderLib::Diagnostics::HrHex(result.profileLevelHr) << "\n";
        std::cout << "  full production-shaped: hr=" << D3DVideoEncoderLib::Diagnostics::HrHex(unbufferedHr) << "\n";
        std::cout << "  full buffered: hr=" << D3DVideoEncoderLib::Diagnostics::HrHex(bufferedHr)
                  << " supportFlags=" << D3DVideoEncoderLib::Diagnostics::Hex64(static_cast<uint64_t>(supportFlags))
                  << " validationFlags=" << D3DVideoEncoderLib::Diagnostics::Hex64(static_cast<uint64_t>(validationFlags))
                  << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Native D3D12 Video Encode minimal probe failed: " << e.what() << "\n";
        return 1;
    }
}
