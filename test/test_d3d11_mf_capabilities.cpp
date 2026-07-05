#include <D3DVideoEncoder/D3D11VideoEncoder.hpp>

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <cstring>

using namespace D3DVideoEncoderLib;

namespace {

void require_true(bool cond, const std::string& message) {
    if (!cond) {
        std::cerr << "FAILED: " << message << "\n";
        std::exit(1);
    }
}

std::string hr_to_string(HRESULT hr) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(8) << std::setfill('0')
        << static_cast<unsigned long>(hr);
    return oss.str();
}

std::wstring widen_ascii(const char* text) {
    std::wstring result;
    if (!text) return result;
    while (*text) {
        result.push_back(static_cast<wchar_t>(*text++));
    }
    return result;
}

void print_support(const char* name, const D3D11MediaFoundationFormatCapability& cap) {
    std::wcout << L"  " << widen_ascii(name)
               << L": supported=" << (cap.supported ? L"yes" : L"no")
               << L", hardware=" << (cap.hardwareSupported ? L"yes" : L"no")
               << L", encoders=" << cap.encoderCount
               << L", hardwareEncoders=" << cap.hardwareEncoderCount;
    if (!cap.firstEncoderName.empty()) {
        std::wcout << L", first=\"" << cap.firstEncoderName << L"\"";
    }
    if (!cap.firstHardwareEncoderName.empty()) {
        std::wcout << L", firstHardware=\"" << cap.firstHardwareEncoderName << L"\"";
    }
    std::wcout << L"\n";

    if (FAILED(cap.queryHr)) {
        std::cerr << "    queryHr=" << hr_to_string(cap.queryHr) << "\n";
    }
    if (FAILED(cap.hardwareQueryHr)) {
        std::cerr << "    hardwareQueryHr=" << hr_to_string(cap.hardwareQueryHr) << "\n";
    }
}

} // namespace

int main() {
    try {
        const auto caps = D3D11VideoEncoder::QueryMediaFoundationCapabilities();

        std::wcout << L"D3D11 Media Foundation encoder capabilities:\n";
        print_support("H264/NV12", caps.h264Nv12);
        print_support("HEVC/NV12", caps.hevcNv12);
        print_support("HEVC/P010", caps.hevcP010);
        print_support("AV1/NV12", caps.av1Nv12);
        print_support("AV1/P010", caps.av1P010);

        require_true(SUCCEEDED(caps.h264Nv12.queryHr), "H.264/NV12 capability query failed");
        require_true(caps.h264Nv12.supported, "H.264/NV12 Media Foundation encoder was not found");

        // HEVC and P010 are optional Windows capabilities. This test verifies that
        // the query path works and reports availability instead of requiring HEVC
        // to be installed on every test machine.
        require_true(SUCCEEDED(caps.hevcNv12.queryHr) || caps.hevcNv12.queryHr == E_INVALIDARG,
                     "HEVC/NV12 capability query returned an unexpected failure");
        require_true(SUCCEEDED(caps.hevcP010.queryHr) || caps.hevcP010.queryHr == E_INVALIDARG,
                     "HEVC/P010 capability query returned an unexpected failure");

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "D3D11 Media Foundation capability test failed: " << e.what() << "\n";
        return 1;
    }
}
