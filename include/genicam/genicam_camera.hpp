#pragma once

#include <arv.h>

#include <cstdint>
#include <string>
#include <vector>

namespace genicam {

struct CameraInfo {
    std::string model;
    std::string serial;
    std::string firmware;
    int width = 0;
    int height = 0;
    double exposure_min_us = 0;
    double exposure_max_us = 0;
    double gain_min = 0;
    double gain_max = 0;
    double temperature_c = 0;
};

struct RawFrame {
    int width = 0;
    int height = 0;
    uint64_t frame_id = 0;
    uint64_t timestamp_ns = 0;
    double exposure_us = 0;
    double gain = 0;
    double temperature_c = 0;
    std::vector<uint8_t> packed;
};

class GenicamCamera {
  public:
    explicit GenicamCamera(const std::string &address);
    ~GenicamCamera();
    GenicamCamera(const GenicamCamera &) = delete;
    GenicamCamera &operator=(const GenicamCamera &) = delete;

    CameraInfo info() const;
    void configure_raw12(double exposure_us, double gain);
    RawFrame capture_raw12();

  private:
    ArvCamera *camera_ = nullptr;
};

std::vector<uint16_t> unpack_mono12_packed(const RawFrame &frame);

} // namespace genicam
