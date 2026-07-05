#pragma once

#include <stdexcept>
#include <string>
#include <Windows.h>

namespace D3DVideoEncoderLib {

class D3DVideoEncoderError : public std::runtime_error {
public:
    explicit D3DVideoEncoderError(const std::string& message)
        : std::runtime_error(message) {}
};

class D3DVideoEncoderHResultError : public D3DVideoEncoderError {
public:
    D3DVideoEncoderHResultError(HRESULT hr, const std::string& message);

    HRESULT hr() const noexcept { return hr_; }

private:
    HRESULT hr_ = S_OK;
};

} // namespace D3DVideoEncoderLib
