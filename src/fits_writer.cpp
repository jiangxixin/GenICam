#include "genicam/fits_writer.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace genicam {
namespace {
std::string card(const std::string &key, const std::string &value,
                 const std::string &comment = {}) {
    std::string result = key;
    result.resize(8, ' ');
    result += "= " + value;
    if (!comment.empty()) result += " / " + comment;
    result.resize(80, ' ');
    return result.substr(0, 80);
}
std::string integer(long long value) {
    std::string text = std::to_string(value);
    text.insert(0, 20 - std::min<size_t>(20, text.size()), ' ');
    return text;
}
std::string real(double value, int precision = 6) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    std::string text = out.str();
    text.insert(0, 20 - std::min<size_t>(20, text.size()), ' ');
    return text;
}
std::string utc_now() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%S", &utc);
    return text;
}
} // namespace

void write_fits_u16(const std::string &path, const RawFrame &frame,
                    const std::vector<uint16_t> &pixels) {
    if (pixels.size() != static_cast<size_t>(frame.width) * frame.height)
        throw std::runtime_error("FITS pixel count mismatch");
    std::string header;
    header += card("SIMPLE", "                   T");
    header += card("BITPIX", integer(16));
    header += card("NAXIS", integer(2));
    header += card("NAXIS1", integer(frame.width));
    header += card("NAXIS2", integer(frame.height));
    header += card("BZERO", integer(32768), "unsigned 16-bit offset");
    header += card("BSCALE", integer(1));
    header += card("DATE-OBS", "'" + utc_now() + "'");
    header += card("EXPTIME", real(frame.exposure_us / 1e6), "seconds");
    header += card("GAIN", real(frame.gain, 3));
    header += card("CCD-TEMP", real(frame.temperature_c, 3), "degrees C");
    header += card("INSTRUME", "'MindVision GEC501M'");
    header += card("SENSOR", "'Sony IMX269AQR1-D'");
    header += card("BAYERPAT", "'NONE'");
    header += card("FRAMEID", integer(frame.frame_id));
    std::string end = "END";
    end.resize(80, ' ');
    header += end;
    header.resize((header.size() + 2879) / 2880 * 2880, ' ');

    std::ofstream out(path, std::ios::binary);
    out.write(header.data(), static_cast<std::streamsize>(header.size()));
    for (uint16_t value : pixels) {
        const uint16_t stored = static_cast<uint16_t>(value - 32768u);
        const char bytes[] = {static_cast<char>(stored >> 8),
                              static_cast<char>(stored & 0xff)};
        out.write(bytes, 2);
    }
    const auto size = out.tellp();
    const int padding = static_cast<int>((2880 - size % 2880) % 2880);
    const char zero = 0;
    for (int i = 0; i < padding; ++i) out.write(&zero, 1);
    if (!out) throw std::runtime_error("failed writing FITS file: " + path);
}
} // namespace genicam
