#include "util/HResult.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>

using namespace D3DVideoEncoderLib;

namespace {
void require_true(bool cond, const char* message) {
    if (!cond) {
        std::cerr << "FAILED: " << message << "\n";
        std::exit(1);
    }
}
}

int main() {
    try {
        CheckHResult(S_OK, "S_OK", __FILE__, __LINE__);

        bool threw = false;
        try {
            CheckHResult(E_INVALIDARG, "E_INVALIDARG", __FILE__, __LINE__, "expected failure");
        } catch (const D3DVideoEncoderHResultError& e) {
            threw = true;
            require_true(e.hr() == E_INVALIDARG, "HRESULT should be preserved");
            require_true(std::string(e.what()).find("expected failure") != std::string::npos, "message should contain caller message");
        }
        require_true(threw, "failing HRESULT should throw");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "unexpected exception: " << e.what() << "\n";
        return 1;
    }
}
