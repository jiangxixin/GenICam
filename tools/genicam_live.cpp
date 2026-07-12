#include <arv.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>

namespace {
std::atomic<bool> running{true};
void stop(int) { running = false; }

[[noreturn]] void fail(const char *operation, GError *error) {
    std::fprintf(stderr, "%s: %s\n", operation,
                 error ? error->message : "unknown error");
    if (error) g_error_free(error);
    std::exit(1);
}
} // namespace

int main(int argc, char **argv) {
    const char *address = argc > 1 ? argv[1] : "192.168.9.104";
    const double exposure_us = argc > 2 ? std::atof(argv[2]) : 2000.0;
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);

    GError *error = nullptr;
    ArvCamera *camera = arv_camera_new(address, &error);
    if (!camera) fail("GigE Vision connection failed", error);

    arv_camera_set_pixel_format_from_string(camera, "Mono8", &error);
    if (error) fail("PixelFormat=Mono8", error);
    arv_camera_set_exposure_time_auto(camera, ARV_AUTO_OFF, &error);
    if (error) fail("ExposureAuto=Off", error);
    arv_camera_set_exposure_time(camera, exposure_us, &error);
    if (error) fail("ExposureTime", error);

    gint x = 0, y = 0, width = 0, height = 0;
    arv_camera_get_region(camera, &x, &y, &width, &height, &error);
    if (error) fail("read image region", error);
    const guint payload = arv_camera_get_payload(camera, &error);
    if (error) fail("read payload size", error);

    ArvStream *stream = arv_camera_create_stream(camera, nullptr, nullptr, &error);
    if (!stream) fail("create GVSP stream", error);
    for (int i = 0; i < 32; ++i)
        arv_stream_push_buffer(stream, arv_buffer_new_allocate(payload));

    arv_camera_start_acquisition(camera, &error);
    if (error) fail("AcquisitionStart", error);
    std::fprintf(stderr,
                 "GenICam live: %s, %dx%d Mono8, %.3f ms, "
                 "payload %u\n",
                 address, width, height, exposure_us / 1000.0, payload);
    setvbuf(stdout, nullptr, _IOFBF, 8 * 1024 * 1024);

    while (running) {
        ArvBuffer *buffer = arv_stream_timeout_pop_buffer(stream, 1'000'000);
        if (!buffer) continue;
        if (arv_buffer_get_status(buffer) == ARV_BUFFER_STATUS_SUCCESS) {
            size_t size = 0;
            const void *data = arv_buffer_get_data(buffer, &size);
            if (size >= static_cast<size_t>(width) * height) {
                if (std::fwrite(data, 1, static_cast<size_t>(width) * height,
                                stdout) != static_cast<size_t>(width) * height) {
                    arv_stream_push_buffer(stream, buffer);
                    break;
                }
                std::fflush(stdout);
            }
        }
        arv_stream_push_buffer(stream, buffer);
    }

    arv_camera_stop_acquisition(camera, nullptr);
    g_object_unref(stream);
    g_object_unref(camera);
    arv_shutdown();
    return 0;
}
