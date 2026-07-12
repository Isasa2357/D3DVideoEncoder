#include "backend/nvenc/NvencCommon.hpp"
#include "backend/nvenc/NvencLifecyclePolicy.hpp"

#include <iostream>
#include <stdexcept>

using namespace D3DVideoEncoderLib;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main() {
    require(!ShouldDrainNvencEos(NV_ENC_SUCCESS, 30, 30), "EOS must not drain when sent equals received");
    require(ShouldDrainNvencEos(NV_ENC_SUCCESS, 30, 29), "EOS must drain a pending packet");
    require(!ShouldDrainNvencEos(NV_ENC_ERR_NEED_MORE_INPUT, 30, 29), "EOS drain requires NV_ENC_SUCCESS");

    NvencEncoderSession unopened;
    unopened.close();
    unopened.close();

    std::cout << "NVENC lifecycle policy tests passed.\n";
    return 0;
}
