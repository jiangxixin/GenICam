cmake_minimum_required(VERSION 3.20)
project(GenICam VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(ARAVIS REQUIRED IMPORTED_TARGET aravis-0.8)

add_executable(genicam-live tools/genicam_live.cpp)
target_link_libraries(genicam-live PRIVATE PkgConfig::ARAVIS)

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
