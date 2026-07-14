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
#include <cstring>
#include <string>
#include <vector>

namespace {
constexpr double kFallbackExposureMinUs = 11.0;
constexpr double kFallbackExposureMaxUs = 5'767'200.0;
constexpr float kZoomMin = 1.0f;
constexpr float kZoomMax = 8.0f;
constexpr int kFocusSourceSize = 192;
constexpr int kFocusViewSize = 384;
constexpr int kRoiAlignment = 4;
constexpr int kMinHardwareRoi = 128;
constexpr int kMinDragPixels = 24;

struct LiveState {
    gint x = 0;
    gint y = 0;
    gint width = 0;
    gint height = 0;
    guint payload = 0;
    ArvStream *stream = nullptr;
    SDL_Texture *texture = nullptr;
    std::vector<unsigned char> mono;
    std::vector<uint32_t> argb;
    bool have_frame = false;
};

struct DeviceInfo {
    std::string id;
    std::string address;
    std::string label;
};

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

int align_down(int value, int alignment) {
    return value - value % alignment;
}

int align_up(int value, int alignment) {
    return align_down(value + alignment - 1, alignment);
}

std::vector<DeviceInfo> discover_devices() {
    arv_update_device_list();
    std::vector<DeviceInfo> devices;
    const unsigned int count = arv_get_n_devices();
    for (unsigned int i = 0; i < count; ++i) {
        const char *id = arv_get_device_id(i);
        const char *address = arv_get_device_address(i);
        const char *vendor = arv_get_device_vendor(i);
        const char *model = arv_get_device_model(i);
        const char *serial = arv_get_device_serial_nbr(i);
        if (!id || !address) continue;

        DeviceInfo info;
        info.id = id;
        info.address = address;
        info.label = std::string(vendor ? vendor : "Camera") + " " +
                     (model ? model : "") + " " +
                     (serial ? serial : "") + " (" + address + ")";
        devices.push_back(std::move(info));
    }
    return devices;
}

std::string select_device_gui() {
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) fail_sdl("SDL_Init");

    SDL_Window *window =
        SDL_CreateWindow("Select GenICam Camera", SDL_WINDOWPOS_CENTERED,
                         SDL_WINDOWPOS_CENTERED, 720, 280,
                         SDL_WINDOW_RESIZABLE);
    if (!window) fail_sdl("SDL_CreateWindow");
    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) fail_sdl("SDL_CreateRenderer");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    std::vector<DeviceInfo> devices = discover_devices();
    int selected = devices.empty() ? -1 : 0;
    std::string chosen_address;
    bool selector_running = true;

    while (selector_running) {
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) selector_running = false;
            if (event.type == SDL_KEYDOWN && !io.WantCaptureKeyboard) {
                if (event.key.keysym.sym == SDLK_ESCAPE) selector_running = false;
            }
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        int window_w = 0;
        int window_h = 0;
        SDL_GetWindowSize(window, &window_w, &window_h);
        ImGui::SetNextWindowPos(ImVec2(16, 16), ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(static_cast<float>(window_w - 32),
                   static_cast<float>(window_h - 32)),
            ImGuiCond_Always);
        ImGui::Begin("Camera", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoResize);

        if (ImGui::Button("Refresh")) {
            devices = discover_devices();
            selected = devices.empty() ? -1 : 0;
        }
        ImGui::SameLine();
        ImGui::Text("%zu device(s)", devices.size());

        const char *preview =
            selected >= 0 && selected < static_cast<int>(devices.size())
                ? devices[selected].label.c_str()
                : "No GenICam camera found";
        if (ImGui::BeginCombo("Device", preview)) {
            for (int i = 0; i < static_cast<int>(devices.size()); ++i) {
                const bool is_selected = i == selected;
                if (ImGui::Selectable(devices[i].label.c_str(), is_selected))
                    selected = i;
                if (is_selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        ImGui::Separator();
        const bool can_connect =
            selected >= 0 && selected < static_cast<int>(devices.size());
        if (!can_connect) ImGui::BeginDisabled();
        if (ImGui::Button("Connect", ImVec2(120, 32))) {
            chosen_address = devices[selected].address;
            selector_running = false;
        }
        if (!can_connect) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(120, 32))) selector_running = false;

        ImGui::TextWrapped(
            "The camera link should be on the local Ethernet subnet. Current "
            "preview will connect by the selected device address.");
        ImGui::End();

        SDL_SetRenderDrawColor(renderer, 8, 10, 14, 255);
        SDL_RenderClear(renderer);
        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return chosen_address;
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

bool set_gain(ArvCamera *camera, double gain) {
    GError *error = nullptr;
    arv_camera_set_gain(camera, gain, &error);
    if (error) {
        std::fprintf(stderr, "Gain %.3f failed: %s\n", gain, error->message);
        g_error_free(error);
        return false;
    }
    return true;
}

double focus_score(const std::vector<unsigned char> &mono, int width, int height,
                   int roi_size) {
    const int half = roi_size / 2;
    const int cx = width / 2;
    const int cy = height / 2;
    const int x0 = std::max(1, cx - half);
    const int x1 = std::min(width - 2, cx + half);
    const int y0 = std::max(1, cy - half);
    const int y1 = std::min(height - 2, cy + half);
    double sum = 0.0;
    int count = 0;
    for (int yy = y0; yy < y1; ++yy) {
        const int row = yy * width;
        for (int xx = x0; xx < x1; ++xx) {
            const int gx = int(mono[row + xx + 1]) - int(mono[row + xx - 1]);
            const int gy =
                int(mono[row + width + xx]) - int(mono[row - width + xx]);
            sum += std::sqrt(double(gx * gx + gy * gy));
            ++count;
        }
    }
    return count > 0 ? sum / count : 0.0;
}

void update_histogram(const std::vector<unsigned char> &mono,
                      std::vector<float> &histogram) {
    std::fill(histogram.begin(), histogram.end(), 0.0f);
    for (unsigned char value : mono) histogram[value] += 1.0f;
    const float max_count =
        std::max(1.0f, *std::max_element(histogram.begin(), histogram.end()));
    for (float &bin : histogram) bin /= max_count;
}

void update_focus_texture(SDL_Texture *texture,
                          const std::vector<unsigned char> &mono, int width,
                          int height, std::vector<uint32_t> &focus_argb) {
    const int src_half = kFocusSourceSize / 2;
    const int src_x0 = width / 2 - src_half;
    const int src_y0 = height / 2 - src_half;
    for (int yy = 0; yy < kFocusViewSize; ++yy) {
        const int src_y = static_cast<int>(
            clamp(src_y0 + yy * kFocusSourceSize / double(kFocusViewSize), 0,
                  height - 1));
        for (int xx = 0; xx < kFocusViewSize; ++xx) {
            const int src_x = static_cast<int>(
                clamp(src_x0 + xx * kFocusSourceSize / double(kFocusViewSize),
                      0, width - 1));
            const uint32_t value =
                mono[static_cast<size_t>(src_y) * width + src_x];
            uint32_t color =
                0xff000000u | (value << 16) | (value << 8) | value;
            if (xx == kFocusViewSize / 2 || yy == kFocusViewSize / 2)
                color = 0xffffd040u;
            focus_argb[static_cast<size_t>(yy) * kFocusViewSize + xx] = color;
        }
    }
    SDL_UpdateTexture(texture, nullptr, focus_argb.data(),
                      kFocusViewSize * int(sizeof(uint32_t)));
}

void update_window_title(SDL_Window *window, const char *address,
                         double exposure_us, float zoom) {
    char title[160];
    std::snprintf(title, sizeof(title),
                  "GenICam Live - %s - exposure %.3f ms - zoom %.2fx", address,
                  exposure_us / 1000.0, zoom);
    SDL_SetWindowTitle(window, title);
}

bool start_stream(ArvCamera *camera, SDL_Renderer *renderer, LiveState &state) {
    GError *error = nullptr;
    state.payload = arv_camera_get_payload(camera, &error);
    if (error) {
        std::fprintf(stderr, "read payload size: %s\n", error->message);
        g_error_free(error);
        return false;
    }
    state.stream = arv_camera_create_stream(camera, nullptr, nullptr, &error);
    if (!state.stream) {
        std::fprintf(stderr, "create GVSP stream: %s\n",
                     error ? error->message : "unknown error");
        if (error) g_error_free(error);
        return false;
    }
    for (int i = 0; i < 32; ++i)
        arv_stream_push_buffer(state.stream,
                               arv_buffer_new_allocate(state.payload));

    state.texture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                          SDL_TEXTUREACCESS_STREAMING, state.width,
                          state.height);
    if (!state.texture) {
        std::fprintf(stderr, "SDL_CreateTexture: %s\n", SDL_GetError());
        g_object_unref(state.stream);
        state.stream = nullptr;
        return false;
    }

    const size_t image_size = static_cast<size_t>(state.width) * state.height;
    state.mono.assign(image_size, 0);
    state.argb.assign(image_size, 0xff000000u);
    state.have_frame = false;

    arv_camera_start_acquisition(camera, &error);
    if (error) {
        std::fprintf(stderr, "AcquisitionStart: %s\n", error->message);
        g_error_free(error);
        SDL_DestroyTexture(state.texture);
        state.texture = nullptr;
        g_object_unref(state.stream);
        state.stream = nullptr;
        return false;
    }
    return true;
}

bool get_full_region_size(ArvCamera *camera, int &width, int &height) {
    GError *error = nullptr;
    gint min_width = 0;
    gint max_width = 0;
    gint min_height = 0;
    gint max_height = 0;
    arv_camera_get_width_bounds(camera, &min_width, &max_width, &error);
    if (error) {
        std::fprintf(stderr, "read width bounds: %s\n", error->message);
        g_error_free(error);
        return false;
    }
    arv_camera_get_height_bounds(camera, &min_height, &max_height, &error);
    if (error) {
        std::fprintf(stderr, "read height bounds: %s\n", error->message);
        g_error_free(error);
        return false;
    }
    width = max_width;
    height = max_height;
    return width > 0 && height > 0;
}

void stop_stream(ArvCamera *camera, LiveState &state) {
    arv_camera_stop_acquisition(camera, nullptr);
    if (state.stream) {
        g_object_unref(state.stream);
        state.stream = nullptr;
    }
    if (state.texture) {
        SDL_DestroyTexture(state.texture);
        state.texture = nullptr;
    }
    state.mono.clear();
    state.argb.clear();
    state.have_frame = false;
}

bool set_centered_region(ArvCamera *camera, int roi_width, int roi_height,
                         LiveState &state) {
    GError *error = nullptr;
    int sensor_width = 0;
    int sensor_height = 0;
    if (!get_full_region_size(camera, sensor_width, sensor_height)) return false;

    roi_width = std::max(1, std::min(roi_width, sensor_width));
    roi_height = std::max(1, std::min(roi_height, sensor_height));
    const int roi_x = std::max(0, (sensor_width - roi_width) / 2);
    const int roi_y = std::max(0, (sensor_height - roi_height) / 2);
    arv_camera_set_region(camera, roi_x, roi_y, roi_width, roi_height, &error);
    if (error) {
        std::fprintf(stderr, "set centered ROI %dx%d: %s\n", roi_width,
                     roi_height, error->message);
        g_error_free(error);
        return false;
    }
    arv_camera_get_region(camera, &state.x, &state.y, &state.width,
                          &state.height, &error);
    if (error) {
        std::fprintf(stderr, "read ROI: %s\n", error->message);
        g_error_free(error);
        return false;
    }
    return true;
}

bool set_absolute_region(ArvCamera *camera, int roi_x, int roi_y, int roi_width,
                         int roi_height, int sensor_width, int sensor_height,
                         LiveState &state) {
    GError *error = nullptr;
    const bool full_frame = roi_x <= 0 && roi_y <= 0 &&
                            roi_width >= sensor_width &&
                            roi_height >= sensor_height;
    if (full_frame) {
        roi_x = 0;
        roi_y = 0;
        roi_width = sensor_width;
        roi_height = sensor_height;
    } else {
        roi_width =
            align_down(std::max(kMinHardwareRoi, roi_width), kRoiAlignment);
        roi_height =
            align_down(std::max(kMinHardwareRoi, roi_height), kRoiAlignment);
        roi_width = std::max(kRoiAlignment, std::min(roi_width, sensor_width));
        roi_height =
            std::max(kRoiAlignment, std::min(roi_height, sensor_height));
        roi_x = align_down(roi_x, kRoiAlignment);
        roi_y = align_down(roi_y, kRoiAlignment);
        roi_x = std::max(0, std::min(roi_x, sensor_width - roi_width));
        roi_y = std::max(0, std::min(roi_y, sensor_height - roi_height));
    }
    arv_camera_set_region(camera, roi_x, roi_y, roi_width, roi_height, &error);
    if (error) {
        std::fprintf(stderr, "set ROI %d,%d %dx%d: %s\n", roi_x, roi_y,
                     roi_width, roi_height, error->message);
        g_error_free(error);
        return false;
    }
    arv_camera_get_region(camera, &state.x, &state.y, &state.width,
                          &state.height, &error);
    if (error) {
        std::fprintf(stderr, "read ROI: %s\n", error->message);
        g_error_free(error);
        return false;
    }
    return true;
}

bool reconfigure_region(ArvCamera *camera, SDL_Renderer *renderer,
                        int roi_width, int roi_height, LiveState &state) {
    stop_stream(camera, state);
    if (!set_centered_region(camera, roi_width, roi_height, state)) return false;
    if (!start_stream(camera, renderer, state)) return false;
    std::fprintf(stderr, "ROI: offset %d,%d, %dx%d, payload %u\n", state.x,
                 state.y, state.width, state.height, state.payload);
    return true;
}

bool reconfigure_absolute_region(ArvCamera *camera, SDL_Renderer *renderer,
                                 int roi_x, int roi_y, int roi_width,
                                 int roi_height, int sensor_width,
                                 int sensor_height, LiveState &state) {
    stop_stream(camera, state);
    if (!set_absolute_region(camera, roi_x, roi_y, roi_width, roi_height,
                             sensor_width, sensor_height, state))
        return false;
    if (!start_stream(camera, renderer, state)) return false;
    std::fprintf(stderr, "ROI: offset %d,%d, %dx%d, payload %u\n", state.x,
                 state.y, state.width, state.height, state.payload);
    return true;
}
} // namespace

int main(int argc, char **argv) {
    std::string selected_address;
    if (argc > 1) {
        selected_address = argv[1];
    } else {
        selected_address = select_device_gui();
        if (selected_address.empty()) {
            arv_shutdown();
            return 0;
        }
    }
    const char *address = selected_address.c_str();
    double exposure_us = argc > 2 ? std::atof(argv[2]) : 10'000.0;
    const int requested_roi_width = argc > 3 ? std::atoi(argv[3]) : 0;
    const int requested_roi_height = argc > 4 ? std::atoi(argv[4]) : 0;
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);

    GError *error = nullptr;
    ArvCamera *camera = arv_camera_new(address, &error);
    if (!camera) fail("GigE Vision connection failed", error);

    arv_camera_set_pixel_format_from_string(camera, "Mono8", &error);
    if (error) fail("PixelFormat=Mono8", error);
    arv_camera_set_exposure_time_auto(camera, ARV_AUTO_OFF, &error);
    if (error) fail("ExposureAuto=Off", error);
    bool gain_available = arv_camera_is_gain_available(camera, &error);
    if (error) {
        std::fprintf(stderr, "Gain availability check failed: %s\n",
                     error->message);
        g_error_free(error);
        error = nullptr;
        gain_available = false;
    }
    if (gain_available) {
        arv_camera_set_gain_auto(camera, ARV_AUTO_OFF, &error);
        if (error) {
            g_error_free(error);
            error = nullptr;
        }
    }

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
    double gain_min = 0.0;
    double gain_max = 0.0;
    double gain = 0.0;
    if (gain_available) {
        arv_camera_get_gain_bounds(camera, &gain_min, &gain_max, &error);
        if (error) {
            std::fprintf(stderr, "Gain bounds unavailable: %s\n", error->message);
            g_error_free(error);
            error = nullptr;
            gain_available = false;
        } else {
            gain = arv_camera_get_gain(camera, &error);
            if (error) {
                std::fprintf(stderr, "Gain read failed: %s\n", error->message);
                g_error_free(error);
                error = nullptr;
                gain = gain_min;
            }
            gain = clamp(gain, gain_min, gain_max);
        }
    }

    int sensor_width = 0;
    int sensor_height = 0;
    if (!get_full_region_size(camera, sensor_width, sensor_height)) return 1;

    LiveState live;
    arv_camera_get_region(camera, &live.x, &live.y, &live.width, &live.height,
                          &error);
    if (error) fail("read image region", error);

    if (requested_roi_width > 0 && requested_roi_height > 0) {
        if (!set_centered_region(camera, requested_roi_width,
                                 requested_roi_height, live))
            return 1;
    } else if (!set_absolute_region(camera, 0, 0, sensor_width, sensor_height,
                                    sensor_width, sensor_height, live)) {
        return 1;
    }

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) fail_sdl("SDL_Init");

    SDL_DisplayMode display{};
    SDL_GetCurrentDisplayMode(0, &display);
    int window_width = std::min(live.width / 2, std::max(960, display.w - 120));
    int window_height = static_cast<int>(
        std::round(window_width * (live.height / double(live.width))));
    if (window_height > display.h - 120) {
        window_height = std::max(540, display.h - 120);
        window_width = static_cast<int>(
            std::round(window_height * (live.width / double(live.height))));
    }

    SDL_Window *window =
        SDL_CreateWindow("GenICam Live", SDL_WINDOWPOS_CENTERED,
                         SDL_WINDOWPOS_CENTERED, window_width, window_height,
                         SDL_WINDOW_RESIZABLE);
    if (!window) fail_sdl("SDL_CreateWindow");
    SDL_Renderer *renderer = SDL_CreateRenderer(
        window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) fail_sdl("SDL_CreateRenderer");
    SDL_Texture *focus_texture =
        SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                          SDL_TEXTUREACCESS_STREAMING, kFocusViewSize,
                          kFocusViewSize);
    if (!focus_texture) fail_sdl("SDL_CreateTexture focus");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    if (!start_stream(camera, renderer, live)) return 1;

    std::fprintf(stderr,
                 "GenICam live: %s, offset %d,%d, %dx%d Mono8, %.3f ms, "
                 "payload %u\n",
                 address, live.x, live.y, live.width, live.height,
                 exposure_us / 1000.0, live.payload);

    std::vector<uint32_t> focus_argb(static_cast<size_t>(kFocusViewSize) *
                                     kFocusViewSize);
    std::vector<float> histogram(256, 0.0f);
    float exposure_ms = static_cast<float>(exposure_us / 1000.0);
    const float exposure_min_ms = static_cast<float>(exposure_min_us / 1000.0);
    const float exposure_max_ms = static_cast<float>(exposure_max_us / 1000.0);
    float gain_value = static_cast<float>(gain);
    float zoom = 1.0f;
    Uint64 last_exposure_write_ms = 0;
    Uint64 last_gain_write_ms = 0;
    double latest_focus_score = 0.0;
    double best_focus_score = 0.0;
    bool dragging_roi = false;
    bool apply_drag_roi = false;
    std::string roi_status = "Drag on video to select ROI";
    SDL_Point roi_drag_start{0, 0};
    SDL_Point roi_drag_end{0, 0};
    SDL_Rect last_image_dst{0, 0, window_width, window_height};
    SDL_Rect last_image_src{0, 0, live.width, live.height};
    SDL_Rect controls_rect{12, 12, std::min(560, window_width - 24), 220};
    SDL_Rect focus_panel_rect{std::max(12, window_width - 424), 12, 412, 452};
    update_window_title(window, address, exposure_us, zoom);

    while (running) {
        apply_drag_roi = false;
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN && !io.WantCaptureKeyboard) {
                const SDL_Keycode key = event.key.keysym.sym;
                if (key == SDLK_q || key == SDLK_ESCAPE) running = false;
            }
            if (event.type == SDL_MOUSEBUTTONDOWN &&
                event.button.button == SDL_BUTTON_LEFT) {
                const SDL_Point p{event.button.x, event.button.y};
                const bool over_ui = SDL_PointInRect(&p, &controls_rect) ||
                                     SDL_PointInRect(&p, &focus_panel_rect);
                const bool over_video = SDL_PointInRect(&p, &last_image_dst);
                if (!over_ui && over_video) {
                    dragging_roi = true;
                    roi_drag_start = p;
                    roi_drag_end = roi_drag_start;
                    roi_status = "Selecting ROI...";
                }
            }
            if (event.type == SDL_MOUSEMOTION && dragging_roi) {
                roi_drag_end = SDL_Point{event.motion.x, event.motion.y};
            }
            if (event.type == SDL_MOUSEBUTTONUP &&
                event.button.button == SDL_BUTTON_LEFT && dragging_roi) {
                roi_drag_end = SDL_Point{event.button.x, event.button.y};
                dragging_roi = false;
                apply_drag_roi = true;
            }
        }

        if (apply_drag_roi) {
            const int x0 = std::min(roi_drag_start.x, roi_drag_end.x);
            const int y0 = std::min(roi_drag_start.y, roi_drag_end.y);
            const int x1 = std::max(roi_drag_start.x, roi_drag_end.x);
            const int y1 = std::max(roi_drag_start.y, roi_drag_end.y);
            const int clipped_x0 = std::max(
                last_image_dst.x, std::min(x0, last_image_dst.x + last_image_dst.w));
            const int clipped_y0 = std::max(
                last_image_dst.y, std::min(y0, last_image_dst.y + last_image_dst.h));
            const int clipped_x1 = std::max(
                last_image_dst.x, std::min(x1, last_image_dst.x + last_image_dst.w));
            const int clipped_y1 = std::max(
                last_image_dst.y, std::min(y1, last_image_dst.y + last_image_dst.h));
            if (clipped_x1 - clipped_x0 >= kMinDragPixels &&
                clipped_y1 - clipped_y0 >= kMinDragPixels) {
                const double scale_x = last_image_src.w / double(last_image_dst.w);
                const double scale_y = last_image_src.h / double(last_image_dst.h);
                const int selected_x =
                    last_image_src.x +
                    int(std::round((clipped_x0 - last_image_dst.x) * scale_x));
                const int selected_y =
                    last_image_src.y +
                    int(std::round((clipped_y0 - last_image_dst.y) * scale_y));
                const int selected_w =
                    std::max(kMinHardwareRoi,
                             align_up(int(std::round((clipped_x1 - clipped_x0) *
                                                     scale_x)),
                                      kRoiAlignment));
                const int selected_h =
                    std::max(kMinHardwareRoi,
                             align_up(int(std::round((clipped_y1 - clipped_y0) *
                                                     scale_y)),
                                      kRoiAlignment));
                if (reconfigure_absolute_region(
                        camera, renderer, live.x + selected_x,
                        live.y + selected_y, selected_w, selected_h,
                        sensor_width, sensor_height, live)) {
                    zoom = 1.0f;
                    best_focus_score = 0.0;
                    latest_focus_score = 0.0;
                    roi_status = "ROI applied";
                } else {
                    roi_status = "ROI apply failed";
                }
            } else {
                roi_status = "ROI ignored: drag a larger box";
            }
        }

        ArvBuffer *buffer = arv_stream_timeout_pop_buffer(live.stream, 30'000);
        if (buffer) {
            if (arv_buffer_get_status(buffer) == ARV_BUFFER_STATUS_SUCCESS) {
                size_t size = 0;
                const unsigned char *data = static_cast<const unsigned char *>(
                    arv_buffer_get_data(buffer, &size));
                const size_t image_size =
                    static_cast<size_t>(live.width) * live.height;
                if (size >= image_size) {
                    std::memcpy(live.mono.data(), data, image_size);
                    for (size_t i = 0; i < image_size; ++i) {
                        const uint32_t v = live.mono[i];
                        live.argb[i] = 0xff000000u | (v << 16) | (v << 8) | v;
                    }
                    SDL_UpdateTexture(live.texture, nullptr, live.argb.data(),
                                      live.width * int(sizeof(uint32_t)));
                    update_histogram(live.mono, histogram);
                    update_focus_texture(focus_texture, live.mono, live.width,
                                         live.height, focus_argb);
                    latest_focus_score = focus_score(live.mono, live.width,
                                                     live.height,
                                                     kFocusSourceSize);
                    best_focus_score =
                        std::max(best_focus_score, latest_focus_score);
                    live.have_frame = true;
                }
            }
            arv_stream_push_buffer(live.stream, buffer);
        }

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        int window_w = 0, window_h = 0;
        SDL_GetWindowSize(window, &window_w, &window_h);
        controls_rect = SDL_Rect{12, 12, std::min(560, window_w - 24), 240};
        focus_panel_rect = SDL_Rect{std::max(12, window_w - 424), 12, 412, 452};
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
        ImGui::SetNextWindowSize(
            ImVec2(static_cast<float>(controls_rect.w),
                   static_cast<float>(controls_rect.h)),
            ImGuiCond_Always);
        ImGui::Begin("GenICam Controls", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove);

        bool exposure_changed = ImGui::SliderFloat(
            "Exposure ms", &exposure_ms, exposure_min_ms, exposure_max_ms,
            "%.3f", ImGuiSliderFlags_Logarithmic);
        const bool exposure_released = ImGui::IsItemDeactivatedAfterEdit();
        bool gain_changed = false;
        bool gain_released = false;
        if (gain_available) {
            gain_changed = ImGui::SliderFloat(
                "Gain", &gain_value, static_cast<float>(gain_min),
                static_cast<float>(gain_max), "%.3f");
            gain_released = ImGui::IsItemDeactivatedAfterEdit();
        } else {
            ImGui::TextDisabled("Gain control unavailable");
        }
        ImGui::SliderFloat("Zoom", &zoom, kZoomMin, kZoomMax, "%.2fx");
        ImGui::Separator();
        ImGui::Text("ROI offset %d,%d size %dx%d", live.x, live.y, live.width,
                    live.height);
        ImGui::Text("%s", roi_status.c_str());
        if (ImGui::Button("Reset ROI")) {
            if (reconfigure_absolute_region(camera, renderer, 0, 0, sensor_width,
                                            sensor_height, sensor_width,
                                            sensor_height, live)) {
                zoom = 1.0f;
                best_focus_score = 0.0;
                latest_focus_score = 0.0;
                roi_status = "ROI reset to full frame";
            }
        }
        ImGui::Text("Focus %.2f   Best %.2f", latest_focus_score,
                    best_focus_score);
        ImGui::SameLine();
        if (ImGui::Button("Reset best")) best_focus_score = latest_focus_score;
        ImGui::PlotHistogram("Histogram", histogram.data(),
                             static_cast<int>(histogram.size()), 0, nullptr,
                             0.0f, 1.0f, ImVec2(0, 54));
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
        if (gain_available && (gain_changed || gain_released)) {
            const double requested_gain =
                clamp(static_cast<double>(gain_value), gain_min, gain_max);
            const Uint64 now = SDL_GetTicks64();
            if (gain_released || now - last_gain_write_ms >= 60) {
                if (set_gain(camera, requested_gain)) gain = requested_gain;
                last_gain_write_ms = now;
            }
        }

        SDL_SetRenderDrawColor(renderer, 8, 10, 14, 255);
        SDL_RenderClear(renderer);

        double crop_w = live.width / static_cast<double>(zoom);
        double crop_h = live.height / static_cast<double>(zoom);
        const double dst_aspect = window_w / double(window_h);
        const double crop_aspect = crop_w / crop_h;
        if (crop_aspect > dst_aspect)
            crop_w = crop_h * dst_aspect;
        else
            crop_h = crop_w / dst_aspect;
        SDL_Rect src{
            static_cast<int>(std::round((live.width - crop_w) / 2.0)),
            static_cast<int>(std::round((live.height - crop_h) / 2.0)),
            static_cast<int>(std::round(crop_w)),
            static_cast<int>(std::round(crop_h)),
        };
        SDL_Rect dst{0, 0, window_w, window_h};
        last_image_src = src;
        last_image_dst = dst;
        if (live.have_frame) SDL_RenderCopy(renderer, live.texture, &src, &dst);
        if (!live.have_frame) {
            ImGui::SetNextWindowPos(ImVec2(12, 248), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(260, 54), ImGuiCond_Always);
            ImGui::Begin("Video status", nullptr,
                         ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove);
            ImGui::Text("Waiting for frame...");
            ImGui::End();
        }
        if (live.have_frame) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 255, 208, 64, 220);
            const int cx = window_w / 2;
            const int cy = window_h / 2;
            SDL_RenderDrawLine(renderer, cx, 0, cx, window_h);
            SDL_RenderDrawLine(renderer, 0, cy, window_w, cy);
            const int box = std::min(window_w, window_h) / 5;
            SDL_Rect roi{cx - box / 2, cy - box / 2, box, box};
            SDL_RenderDrawRect(renderer, &roi);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        }

        if (dragging_roi) {
            const int x0 = std::min(roi_drag_start.x, roi_drag_end.x);
            const int y0 = std::min(roi_drag_start.y, roi_drag_end.y);
            const int x1 = std::max(roi_drag_start.x, roi_drag_end.x);
            const int y1 = std::max(roi_drag_start.y, roi_drag_end.y);
            SDL_Rect selected{x0, y0, x1 - x0, y1 - y0};
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(renderer, 70, 170, 255, 70);
            SDL_RenderFillRect(renderer, &selected);
            SDL_SetRenderDrawColor(renderer, 70, 170, 255, 240);
            SDL_RenderDrawRect(renderer, &selected);
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
        }

        if (live.have_frame) {
            ImGui::SetNextWindowPos(
                ImVec2(static_cast<float>(focus_panel_rect.x),
                       static_cast<float>(focus_panel_rect.y)),
                ImGuiCond_Always);
            ImGui::SetNextWindowSize(
                ImVec2(static_cast<float>(focus_panel_rect.w),
                       static_cast<float>(focus_panel_rect.h)),
                ImGuiCond_Always);
            ImGui::Begin("Focus ROI", nullptr,
                         ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove);
            ImGui::Text("Center 2x view");
            ImGui::Image(reinterpret_cast<ImTextureID>(focus_texture),
                         ImVec2(kFocusViewSize, kFocusViewSize));
            ImGui::End();
        }

        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    stop_stream(camera, live);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyTexture(focus_texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    g_object_unref(camera);
    arv_shutdown();
    return 0;
}
