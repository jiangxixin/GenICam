#include "genicam/genicam_camera.hpp"

#include <array>
#include <cstdint>
#include <cstdio>

int main() {
    constexpr std::array<uint16_t, 6> expected{0x000, 0x001, 0x123,
                                               0xabc, 0xfff, 0x800};
    genicam::RawFrame frame;
    frame.width = 6;
    frame.height = 1;
    for (size_t i = 0; i < expected.size(); i += 2) {
        const uint16_t p0 = expected[i];
        const uint16_t p1 = expected[i + 1];
        frame.packed.push_back(static_cast<uint8_t>(p0));
        frame.packed.push_back(static_cast<uint8_t>(((p1 & 0x0f) << 4) |
                                                    ((p0 >> 8) & 0x0f)));
        frame.packed.push_back(static_cast<uint8_t>(p1 >> 4));
    }
    const auto actual = genicam::unpack_mono12_packed(frame);
    if (!std::equal(actual.begin(), actual.end(), expected.begin())) {
        std::fprintf(stderr, "Mono12Packed unpack test failed\n");
        return 1;
    }
    return 0;
}
