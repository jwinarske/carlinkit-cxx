cmake_minimum_required(VERSION 3.16)
project(carlinkit-cxx LANGUAGES CXX)

# C++17 to match drm-cxx's ABI (its drm::span = tcb::span at C++17; C++20 would
# select std::span and mismatch its compiled symbols).
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
find_package(Threads REQUIRED)

# Stage 1: headless USB + protocol verification (no display deps).
add_executable(carlinkit-probe
  src/main.cpp
  src/usb_device.cpp
  src/dongle.cpp
)
target_link_libraries(carlinkit-probe PRIVATE PkgConfig::LIBUSB Threads::Threads)
target_compile_options(carlinkit-probe PRIVATE -Wall -Wextra)

# Headless verification of the libavcodec+VAAPI -> DRM-PRIME zero-copy decode
# path (no DRM master needed). See docs/ARCHITECTURE.md.
pkg_check_modules(LIBAV IMPORTED_TARGET libavcodec libavutil)
pkg_check_modules(LIBVA IMPORTED_TARGET libva libva-drm)
pkg_check_modules(ALSA IMPORTED_TARGET alsa)
pkg_check_modules(LIBDRM_DUMP IMPORTED_TARGET libdrm)

# Read-only DRM plane/modifier topology dumper (no DRM master needed).
if(LIBDRM_DUMP_FOUND)
  add_executable(carlinkit-drm-dump src/drm_dump.cpp)
  target_link_libraries(carlinkit-drm-dump PRIVATE PkgConfig::LIBDRM_DUMP)
  target_compile_options(carlinkit-drm-dump PRIVATE -Wall -Wextra)
endif()

# Headless audio test: dongle PCM -> ALSA playback + mic capture (no display).
if(ALSA_FOUND)
  add_executable(carlinkit-audio src/main_audio.cpp src/audio_alsa.cpp src/usb_device.cpp
                                 src/dongle.cpp)
  target_link_libraries(carlinkit-audio PRIVATE PkgConfig::ALSA PkgConfig::LIBUSB Threads::Threads)
  target_compile_options(carlinkit-audio PRIVATE -Wall -Wextra)
endif()
if(LIBAV_FOUND AND LIBVA_FOUND)
  add_executable(carlinkit-decode-probe src/decode_probe.cpp src/vaapi_decoder.cpp)
  target_link_libraries(carlinkit-decode-probe PRIVATE PkgConfig::LIBAV PkgConfig::LIBVA)
  target_compile_options(carlinkit-decode-probe PRIVATE -Wall -Wextra)
endif()

# Stage 2b: KMS app — drm-cxx scene + HW plane, fed by the VAAPI decoder.
# drm-cxx is consumed as a sibling source tree via add_subdirectory.
set(DRM_CXX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../drm-cxx" CACHE PATH "Path to drm-cxx source tree")
pkg_check_modules(LIBDRM IMPORTED_TARGET libdrm)
if(LIBAV_FOUND AND LIBVA_FOUND AND LIBDRM_FOUND AND EXISTS "${DRM_CXX_DIR}/CMakeLists.txt")
  # Trim drm-cxx to what we need (no gstreamer/examples/tests/install).
  set(DRM_CXX_GSTREAMER OFF CACHE STRING "" FORCE)
  set(DRM_CXX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(DRM_CXX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(DRM_CXX_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  set(DRM_CXX_INSTALL OFF CACHE BOOL "" FORCE)
  add_subdirectory(${DRM_CXX_DIR} ${CMAKE_BINARY_DIR}/drm-cxx-build)

  add_executable(carlinkit-kms
    src/main_kms.cpp
    src/decoder_source.cpp
    src/vaapi_decoder.cpp
    src/vaapi_decoder_source.cpp
    src/audio_alsa.cpp
    src/usb_device.cpp
    src/dongle.cpp
    src/dongle_manager.cpp
  )
  target_include_directories(carlinkit-kms PRIVATE ${DRM_CXX_DIR}/examples/common)
  target_link_libraries(carlinkit-kms PRIVATE
    drm-cxx::drm-cxx PkgConfig::LIBAV PkgConfig::LIBVA PkgConfig::LIBDRM
    PkgConfig::ALSA PkgConfig::LIBUSB Threads::Threads)
  target_compile_options(carlinkit-kms PRIVATE -Wall -Wextra)
else()
  message(STATUS "carlinkit-kms skipped (need libav, libva, libdrm, and drm-cxx at ${DRM_CXX_DIR})")
endif()
