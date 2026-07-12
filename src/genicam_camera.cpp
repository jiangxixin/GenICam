#include "genicam/genicam_camera.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace genicam {
namespace {

void check(GError *error, const char *operation) {
    if (!error) return;
    std::string message = std::string(operation) + ": " + error->message;
    g_error_free(error);
    throw std::runtime_error(message);
}

std::string camera_string(ArvCamera *camera, const char *feature) {
    GError *error = nullptr;
    const char *value = arv_camera_get_string(camera, feature, &error);
    check(error, feature);
    return value ? value : "";
}

double camera_float(ArvCamera *camera, const char *feature) {
    GError *error = nullptr;
    const double value = arv_camera_get_float(camera, feature, &error);
    check(error, feature);
    return value;
}

} // namespace

GenicamCamera::GenicamCamera(const std::string &address) {
    GError *error = nullptr;
    camera_ = arv_camera_new(address.c_str(), &error);
    check(error, "connect GigE Vision camera");
    if (!camera_) throw std::runtime_error("camera not found at " + address);
}

GenicamCamera::~GenicamCamera() {
    if (camera_) g_object_unref(camera_);
}

CameraInfo GenicamCamera::info() const {
    CameraInfo result;
    GError *identity_error = nullptr;
    const char *model = arv_camera_get_model_name(camera_, &identity_error);
    check(identity_error, "read model name");
    const char *serial = arv_camera_get_device_serial_number(camera_, &identity_error);
    check(identity_error, "read serial number");
    result.model = model ? model : "";
    result.serial = serial ? serial : "";
    result.firmware = camera_string(camera_, "DeviceFirmwareVersion");
    GError *error = nullptr;
    gint x = 0, y = 0;
    arv_camera_get_region(camera_, &x, &y, &result.width, &result.height, &error);
    check(error, "read Width/Height");
    arv_camera_get_exposure_time_bounds(camera_, &result.exposure_min_us,
                                        &result.exposure_max_us, &error);
    check(error, "read exposure bounds");
    arv_camera_get_gain_bounds(camera_, &result.gain_min, &result.gain_max, &error);
    check(error, "read gain bounds");
    result.temperature_c = camera_float(camera_, "DeviceTemperature");
    return result;
}

void GenicamCamera::configure_raw12(double exposure_us, double gain) {
    GError *error = nullptr;
    arv_camera_set_pixel_format_from_string(camera_, "Mono12Packed", &error);
    check(error, "PixelFormat=Mono12Packed");
    arv_camera_set_exposure_time_auto(camera_, ARV_AUTO_OFF, &error);
    check(error, "ExposureAuto=Off");
    arv_camera_set_exposure_time(camera_, exposure_us, &error);
    check(error, "set ExposureTime");
    arv_camera_set_gain(camera_, gain, &error);
    check(error, "set Gain");
    arv_camera_set_string(camera_, "AcquisitionMode", "Continuous", &error);
    check(error, "AcquisitionMode=Continuous");
    arv_camera_set_string(camera_, "AcquisitionFrameRate", "Low", &error);
    check(error, "AcquisitionFrameRate=Low");
    arv_camera_set_boolean(camera_, "AcquisitionFrameRateEnable", FALSE, &error);
    check(error, "AcquisitionFrameRateEnable=false");
    arv_camera_set_string(camera_, "TriggerMode", "On", &error);
    check(error, "TriggerMode=On");
    arv_camera_set_integer(camera_, "TriggerCount", 1, &error);
    check(error, "TriggerCount=1");
}

RawFrame GenicamCamera::capture_raw12() {
    GError *error = nullptr;
    gint x = 0, y = 0, width = 0, height = 0;
    arv_camera_get_region(camera_, &x, &y, &width, &height, &error);
    check(error, "read frame dimensions");
    const guint payload = arv_camera_get_payload(camera_, &error);
    check(error, "read PayloadSize");
    ArvStream *stream = arv_camera_create_stream(camera_, nullptr, nullptr, &error);
    check(error, "create GVSP stream");
    if (!stream) throw std::runtime_error("failed to create GVSP stream");
    for (int i = 0; i < 16; ++i)
        arv_stream_push_buffer(stream, arv_buffer_new_allocate(payload));

    arv_camera_start_acquisition(camera_, &error);
    check(error, "AcquisitionStart");
    const double exposure_us = arv_camera_get_exposure_time(camera_, &error);
    check(error, "read ExposureTime");
    const guint64 timeout_us = static_cast<guint64>(exposure_us + 10'000'000.0);
    g_usleep(100'000); // allow the stream socket and camera trigger FSM to settle
    ArvBuffer *buffer = nullptr;
    int last_status = ARV_BUFFER_STATUS_UNKNOWN;
    for (int attempt = 0; attempt < 4; ++attempt) {
        arv_camera_execute_command(camera_, "TriggerSoftware", &error);
        check(error, "TriggerSoftware");
        buffer = arv_stream_timeout_pop_buffer(stream, timeout_us);
        if (buffer && arv_buffer_get_status(buffer) == ARV_BUFFER_STATUS_SUCCESS)
            break;
        if (buffer) {
            last_status = arv_buffer_get_status(buffer);
            arv_stream_push_buffer(stream, buffer);
            buffer = nullptr;
        }
        g_usleep(50'000);
    }
    arv_camera_stop_acquisition(camera_, nullptr);
    if (!buffer) {
        g_object_unref(stream);
        throw std::runtime_error("timed out waiting for triggered frame");
    }
    if (arv_buffer_get_status(buffer) != ARV_BUFFER_STATUS_SUCCESS)
        throw std::runtime_error("incomplete GVSP frame, status=" +
                                 std::to_string(last_status));

    size_t size = 0;
    const auto *data = static_cast<const uint8_t *>(arv_buffer_get_data(buffer, &size));
    RawFrame frame;
    frame.width = arv_buffer_get_image_width(buffer);
    frame.height = arv_buffer_get_image_height(buffer);
    frame.frame_id = arv_buffer_get_frame_id(buffer);
    frame.timestamp_ns = arv_buffer_get_timestamp(buffer);
    frame.exposure_us = exposure_us;
    frame.gain = arv_camera_get_gain(camera_, &error);
    check(error, "read Gain");
    frame.temperature_c = camera_float(camera_, "DeviceTemperature");
    frame.packed.assign(data, data + size);
    g_object_unref(buffer);
    g_object_unref(stream);
    return frame;
}

std::vector<uint16_t> unpack_mono12_packed(const RawFrame &frame) {
    const size_t pixels = static_cast<size_t>(frame.width) * frame.height;
    if (frame.packed.size() != pixels * 3 / 2 || pixels % 2 != 0)
        throw std::runtime_error("invalid Mono12Packed payload size");
    std::vector<uint16_t> output(pixels);
    size_t p = 0;
    for (size_t i = 0; i < frame.packed.size(); i += 3) {
        output[p++] = frame.packed[i] | ((frame.packed[i + 1] & 0x0f) << 8);
        output[p++] = (frame.packed[i + 2] << 4) | (frame.packed[i + 1] >> 4);
    }
    return output;
}

} // namespace genicam
