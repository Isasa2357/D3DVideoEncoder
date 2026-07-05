#pragma once

#include <string>
#include <Windows.h>

#include <D3DVideoEncoder/D3DVideoEncoderError.hpp>

namespace D3DVideoEncoderLib {

std::string HResultToString(HRESULT hr);
[[noreturn]] void ThrowHResult(HRESULT hr, const char* expression, const char* file, int line, const std::string& message = {});
void CheckHResult(HRESULT hr, const char* expression, const char* file, int line, const std::string& message = {});

} // namespace D3DVideoEncoderLib

#define D3DVE_THROW_IF_FAILED(expr) \
    ::D3DVideoEncoderLib::CheckHResult((expr), #expr, __FILE__, __LINE__)

#define D3DVE_THROW_IF_FAILED_MSG(expr, msg) \
    ::D3DVideoEncoderLib::CheckHResult((expr), #expr, __FILE__, __LINE__, (msg))
