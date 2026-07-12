#include "genicam/fits_writer.hpp"
#include "genicam/genicam_camera.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) {
        std::fprintf(stderr,
                     "usage: %s EXPOSURE_SECONDS OUTPUT.fits [GAIN] [IP]\n",
                     argv[0]);
        return 2;
    }
    const double exposure_us = std::atof(argv[1]) * 1e6;
    const std::string output = argv[2];
    const double gain = argc > 3 ? std::atof(argv[3]) : 1.0;
    const std::string address = argc > 4 ? argv[4] : "192.168.9.104";
    try {
        genicam::GenicamCamera camera(address);
        const auto info = camera.info();
        std::printf("%s serial=%s firmware=%s %dx%d temp=%.2f C\n",
                    info.model.c_str(), info.serial.c_str(), info.firmware.c_str(),
                    info.width, info.height, info.temperature_c);
        camera.configure_raw12(exposure_us, gain);
        const auto frame = camera.capture_raw12();
        const auto pixels = genicam::unpack_mono12_packed(frame);
        const auto [min_it, max_it] = std::minmax_element(pixels.begin(), pixels.end());
        genicam::write_fits_u16(output, frame, pixels);
        std::printf("saved %s frame=%llu exposure=%.6f s gain=%.3f "
                    "temp=%.2f C range=%u..%u\n",
                    output.c_str(), static_cast<unsigned long long>(frame.frame_id),
                    frame.exposure_us / 1e6, frame.gain, frame.temperature_c,
                    *min_it, *max_it);
    } catch (const std::exception &error) {
        std::fprintf(stderr, "genicam-capture: %s\n", error.what());
        return 1;
    }
    return 0;
}
