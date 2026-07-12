#pragma once

#include "genicam/genicam_camera.hpp"

#include <string>
#include <vector>

namespace genicam {
void write_fits_u16(const std::string &path, const RawFrame &frame,
                    const std::vector<uint16_t> &pixels);
} // namespace genicam
