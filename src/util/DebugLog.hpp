#pragma once

#include <string>
#include <string_view>

namespace D3DVideoEncoderLib {

class DebugLog {
public:
    DebugLog() = default;
    explicit DebugLog(bool enabled) : enabled_(enabled) {}

    bool enabled() const noexcept { return enabled_; }
    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }

    void info(std::string_view message) const;
    void info(const std::wstring& message) const;

private:
    bool enabled_ = false;
};

} // namespace D3DVideoEncoderLib
