#define SDL_MAIN_HANDLED

#include <SDL.h>
#include <arv.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
constexpr double kFallbackExposureMinUs = 11.0;
constexpr double kFallbackExposureMaxUs = 5'767'200.0;
constexpr float kZoomMin = 1.0f;
constexpr float kZoomMax = 8.0f;

std::atomic<bool> running{true};
void stop(int) { running = false; }

[[noreturn]] void fail(const char *operation, GError *error) {
    std::fprintf(stderr, "%s: %s\n", operation,
                 error ? error->message : "unknown error");
    if (error) g_error_free(error);
    std::exit(1);
}

[[noreturn]] void fail_sdl(const char *operation) {
    std::fprintf(stderr, "%s: %s\n", operation, SDL_GetError());
    std::exit(1);
}

double clamp(double value, double min_value, double max_value) {
    return std::max(min_value, std::min(max_value, value));
}

bool set_exposure(ArvCamera *camera, double exposure_us) {
    GError *error = nullptr;
    arv_camera_set_exposure_time(camera, exposure_us, &error);
    if (error) {
        std::fprintf(stderr, "ExposureTime %.0f us failed: %s\n", exposure_us,
                     error->message);
        g_error_free(error);
        return false;
    }
    return true;
}

void update_window_title(SDL_Window *window, const char *address,
                         double exposure_us, float zoom) {
    char title[160];
    std::snprintf(title, sizeof(title),
                  "GenICam Live - %s - exposure %.3f ms - zoom %.2fx", address,
                  exposure_us / 1000.0, zoom);
    SDL_SetWindowTitle(window, title);
}
} // namespace

int main(int argc, char **argv) {
    const char *address = argc > 1 ? argv[1] : "192.168.9.104";
    double exposure_us = argc > 2 ? std::atof(argv[2]) : 10'000.0;
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);

    GError *error = nullptr;
    ArvCamera *camera = arv_camera_new(address, &error);
    if (!camera) fail("GigE Vision connection failed", error);

    arv_camera_set_pixel_format_from_string(camera, "Mono8", &error);
    if (error) fail("PixelFormat=Mono8", error);
    arv_camera_set_exposure_time_auto(camera, ARV_AUTO_OFF, &error);
    if (error) fail("ExposureAuto=Off", error);

    double exposure_min_us = kFallbackExposureMinUs;
    double exposure_max_us = kFallbackExposureMaxUs;
    arv_camera_get_exposure_time_bounds(camera, &exposure_min_us,
                                        &exposure_max_us, &error);
    if (error) {
        std::fprintf(stderr,
                     "Exposure bounds unavailable, using %.0f..%.0f us: %s\n",
                     kFallbackExposureMinUs, kFallbackExposureMaxUs,
                     error->message);
        g_error_free(error);
        error = nullptr;
        exposure_min_us = kFallbackExposureMinUs;
        exposure_max_us = kFallbackExposureMaxUs;
    }
    exposure_us = clamp(exposure_us, exposure_min_us, exposure_max_us);
    if (!set_exposure(camera, exposure_us)) return 1;

    gint x = 0, y = 0, width = 0, height = 0;
    arv_camera_get_region(camera, &x, &y, &width, &height, &error);
    if (error) fail("read image region", error);
    const guint payload = arv_camera_get_payload(camera, &error);
    if (error) fail("read payload size", error);

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) fail_sdl("SDL_Init");

    SDL_DisplayMode display{};
    SDL_GetCurrentDisplayMode(0, &display);
    int window_width = std::min(width / 2, std::max(960, display.w - 120));
    int window_height =
        static_cast<int>(std::round(window_width * (height / double(width))));
    if (window_height > display.h - 120) {
        window_height = std::max(540, display.h - 120);
        window_width =
            static_cast<int>(std::round(window_height * (width / double(height))));
    }

    SDL_Window *window =
        SDL_CreateWindow("GenICam Live", SDL_WINDOWPOS_CENTERED,
                         SDL_WINDOWPOS_CENTERED, window_width, window_height,
                         SDL_WINDOW_RESIZABLE);
    if (!window) fail_sdl("SDL_CreateWindow");
    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) fail_sdl("SDL_CreateRenderer");
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                             SDL_TEXTUREACCESS_STREAMING, width,
                                             height);
    if (!texture) fail_sdl("SDL_CreateTexture");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    ArvStream *stream = arv_camera_create_stream(camera, nullptr, nullptr, &error);
    if (!stream) fail("create GVSP stream", error);
    for (int i = 0; i < 32; ++i)
        arv_stream_push_buffer(stream, arv_buffer_new_allocate(payload));

    arv_camera_start_acquisition(camera, &error);
    if (error) fail("AcquisitionStart", error);

    std::fprintf(stderr,
                 "GenICam live: %s, %dx%d Mono8, %.3f ms, payload %u\n",
                 address, width, height, exposure_us / 1000.0, payload);

    std::vector<uint32_t> argb(static_cast<size_t>(width) * height);
    float exposure_ms = static_cast<float>(exposure_us / 1000.0);
    const float exposure_min_ms = static_cast<float>(exposure_min_us / 1000.0);
    const float exposure_max_ms = static_cast<float>(exposure_max_us / 1000.0);
    float zoom = 1.0f;
    Uint64 last_exposure_write_ms = 0;
    bool have_frame = false;
    update_window_title(window, address, exposure_us, zoom);

    while (running) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN && !io.WantCaptureKeyboard) {
                const SDL_Keycode key = event.key.keysym.sym;
                if (key == SDLK_q || key == SDLK_ESCAPE) running = false;
            }
        }

        ArvBuffer *buffer = arv_stream_timeout_pop_buffer(stream, 30'000);
        if (buffer) {
            if (arv_buffer_get_status(buffer) == ARV_BUFFER_STATUS_SUCCESS) {
                size_t size = 0;
                const unsigned char *data = static_cast<const unsigned char *>(
                    arv_buffer_get_data(buffer, &size));
                const size_t image_size = static_cast<size_t>(width) * height;
                if (size >= image_size) {
                    for (size_t i = 0; i < image_size; ++i) {
                        const uint32_t v = data[i];
                        argb[i] = 0xff000000u | (v << 16) | (v << 8) | v;
                    }
                    SDL_UpdateTexture(texture, nullptr, argb.data(),
                                      width * int(sizeof(uint32_t)));
                    have_frame = true;
                }
            }
            arv_stream_push_buffer(stream, buffer);
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        int window_w = 0, window_h = 0;
        SDL_GetWindowSize(window, &window_w, &window_h);
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(std::min(560, window_w - 24), 128),
                                 ImGuiCond_Always);
        ImGui::Begin("GenICam Controls", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove);

        bool exposure_changed = ImGui::SliderFloat(
            "Exposure ms", &exposure_ms, exposure_min_ms, exposure_max_ms,
            "%.3f", ImGuiSliderFlags_Logarithmic);
        const bool exposure_released = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Zoom", &zoom, kZoomMin, kZoomMax, "%.2fx");
        ImGui::Text("q / Esc: quit");
        ImGui::End();

        if (exposure_changed || exposure_released) {
            const double requested_us =
                clamp(static_cast<double>(exposure_ms) * 1000.0,
                      exposure_min_us, exposure_max_us);
            const Uint64 now = SDL_GetTicks64();
            if (exposure_released || now - last_exposure_write_ms >= 60) {
                if (set_exposure(camera, requested_us)) {
                    exposure_us = requested_us;
                    update_window_title(window, address, exposure_us, zoom);
                }
                last_exposure_write_ms = now;
            }
        }

        SDL_SetRenderDrawColor(renderer, 8, 10, 14, 255);
        SDL_RenderClear(renderer);

        double crop_w = width / static_cast<double>(zoom);
        double crop_h = height / static_cast<double>(zoom);
        const double dst_aspect = window_w / double(window_h);
        const double crop_aspect = crop_w / crop_h;
        if (crop_aspect > dst_aspect)
            crop_w = crop_h * dst_aspect;
        else
            crop_h = crop_w / dst_aspect;
        SDL_Rect src{
            static_cast<int>(std::round((width - crop_w) / 2.0)),
            static_cast<int>(std::round((height - crop_h) / 2.0)),
            static_cast<int>(std::round(crop_w)),
            static_cast<int>(std::round(crop_h)),
        };
        SDL_Rect dst{0, 0, window_w, window_h};
        if (have_frame) SDL_RenderCopy(renderer, texture, &src, &dst);

        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    arv_camera_stop_acquisition(camera, nullptr);
    g_object_unref(stream);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    g_object_unref(camera);
    arv_shutdown();
    return 0;
}
