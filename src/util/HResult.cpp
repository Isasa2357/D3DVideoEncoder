#include "util/HResult.hpp"

#include <sstream>

namespace D3DVideoEncoderLib {

D3DVideoEncoderHResultError::D3DVideoEncoderHResultError(HRESULT hr, const std::string& message)
    : D3DVideoEncoderError(message), hr_(hr) {}

std::string HResultToString(HRESULT hr) {
    char* buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;

    const DWORD length = FormatMessageA(
        flags,
        nullptr,
        static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer),
        0,
        nullptr);

    std::ostringstream oss;
    oss << "HRESULT=0x" << std::hex << static_cast<unsigned long>(hr);
    if (length != 0 && buffer != nullptr) {
        oss << " (" << buffer << ")";
        LocalFree(buffer);
    }
    return oss.str();
}

[[noreturn]] void ThrowHResult(HRESULT hr, const char* expression, const char* file, int line, const std::string& message) {
    std::ostringstream oss;
    if (!message.empty()) {
        oss << message << ": ";
    }
    oss << expression << " failed at " << file << ":" << line << " - " << HResultToString(hr);
    throw D3DVideoEncoderHResultError(hr, oss.str());
}

void CheckHResult(HRESULT hr, const char* expression, const char* file, int line, const std::string& message) {
    if (FAILED(hr)) {
        ThrowHResult(hr, expression, file, line, message);
    }
}

} // namespace D3DVideoEncoderLib
