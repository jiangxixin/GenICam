cmake_minimum_required(VERSION 3.20)
project(GenICam VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ARAVIS REQUIRED IMPORTED_TARGET aravis-0.8)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

include(FetchContent)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.9b
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui)

add_library(imgui-sdl2 STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp)
target_include_directories(imgui-sdl2 PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui-sdl2 PUBLIC PkgConfig::SDL2)

add_executable(genicam-live tools/genicam_live.cpp)
target_link_libraries(genicam-live PRIVATE PkgConfig::ARAVIS imgui-sdl2)

if(APPLE)
  add_executable(genicam-live-app MACOSX_BUNDLE tools/genicam_live.cpp)
  target_link_libraries(genicam-live-app PRIVATE PkgConfig::ARAVIS imgui-sdl2)
  set_target_properties(genicam-live-app PROPERTIES
    OUTPUT_NAME "GenICam Live"
    MACOSX_BUNDLE_BUNDLE_NAME "GenICam Live"
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.jiangxixin.genicam.live"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}")
endif()

add_library(genicam-driver
  src/genicam_camera.cpp
  src/fits_writer.cpp)
target_include_directories(genicam-driver PUBLIC include)
target_link_libraries(genicam-driver PUBLIC PkgConfig::ARAVIS)

add_executable(genicam-capture apps/genicam_capture.cpp)
target_link_libraries(genicam-capture PRIVATE genicam-driver)

include(CTest)
if(BUILD_TESTING)
  add_executable(test-unpack tests/test_unpack.cpp)
  target_link_libraries(test-unpack PRIVATE genicam-driver)
  add_test(NAME mono12-packed-unpack COMMAND test-unpack)
endif()
