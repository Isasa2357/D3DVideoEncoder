#include "util/DebugLog.hpp"

#include <iostream>

namespace D3DVideoEncoderLib {

void DebugLog::info(std::string_view message) const {
    if (!enabled_) return;
    std::cout << "[D3DVideoEncoder] " << message << std::endl;
}

void DebugLog::info(const std::wstring& message) const {
    if (!enabled_) return;
    std::wcout << L"[D3DVideoEncoder] " << message << std::endl;
}

} // namespace D3DVideoEncoderLib
