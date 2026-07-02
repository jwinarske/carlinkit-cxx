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
pkg_check_modules(LIBSWS IMPORTED_TARGET libswscale)
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

# KMS app — drm-cxx scene + HW plane, fed by the decoder. drm-cxx is pinned as a
# git submodule (third_party/drm-cxx) and consumed via add_subdirectory; override
# DRM_CXX_DIR to build against a local working tree instead.
set(DRM_CXX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/drm-cxx" CACHE PATH "Path to drm-cxx source tree")
pkg_check_modules(LIBDRM IMPORTED_TARGET libdrm)
# EGL + GLESv2 back the GPU rotate/convert fallback for angles a plane can't do
# (90/270 on the Pi's vc6). Optional: absent -> the software rotate stays the
# only fallback.
pkg_check_modules(EGL IMPORTED_TARGET egl)
pkg_check_modules(GLES IMPORTED_TARGET glesv2)
# Force the CPU-only decode path, omitting the VAAPI and V4L2 backends (and
# their libva dependency) from the build to minimize code size. The runtime
# CARLINKIT_DECODER override is ignored in this build.
option(CARLINKIT_SOFTWARE_ONLY "Build only the software (CPU) H.264 decoder" OFF)

if(LIBAV_FOUND AND LIBSWS_FOUND AND LIBDRM_FOUND AND EXISTS "${DRM_CXX_DIR}/CMakeLists.txt"
   AND (CARLINKIT_SOFTWARE_ONLY OR LIBVA_FOUND))
  # Trim drm-cxx to what we need (no gstreamer/examples/tests/install).
  set(DRM_CXX_GSTREAMER OFF CACHE STRING "" FORCE)
  set(DRM_CXX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(DRM_CXX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(DRM_CXX_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
  set(DRM_CXX_INSTALL OFF CACHE BOOL "" FORCE)
  # drm-cxx's vendored deps (tcb-span, tl-expected, fmt, ...) are meson wrap
  # subprojects that are git-ignored in the submodule; provision them once if
  # absent so the add_subdirectory build finds their headers.
  if(NOT EXISTS "${DRM_CXX_DIR}/subprojects/tcb-span/include")
    find_program(MESON_EXECUTABLE meson)
    if(MESON_EXECUTABLE)
      message(STATUS "drm-cxx: downloading meson subprojects via ${MESON_EXECUTABLE}")
      execute_process(COMMAND ${MESON_EXECUTABLE} subprojects download
                      WORKING_DIRECTORY ${DRM_CXX_DIR})
    else()
      message(WARNING "meson not found; run 'meson subprojects download' in ${DRM_CXX_DIR}")
    endif()
  endif()
  # The GLES rotate stage needs drm-cxx's GlScanoutProducer (EGL); force it on
  # when EGL/GLESv2 are present so it compiles into libdrm-cxx.
  if(EGL_FOUND AND GLES_FOUND)
    set(DRM_CXX_EGL ON CACHE STRING "" FORCE)
  endif()
  add_subdirectory(${DRM_CXX_DIR} ${CMAKE_BINARY_DIR}/drm-cxx-build)

  set(KMS_SOURCES
    src/main_kms.cpp
    src/decoder_source.cpp
    src/software_decoder_source.cpp
    src/audio_alsa.cpp
    src/usb_device.cpp
    src/dongle.cpp
    src/dongle_manager.cpp
  )
  set(KMS_LIBS
    drm-cxx::drm-cxx PkgConfig::LIBAV PkgConfig::LIBSWS PkgConfig::LIBDRM
    PkgConfig::ALSA PkgConfig::LIBUSB Threads::Threads)
  if(NOT CARLINKIT_SOFTWARE_ONLY)
    list(APPEND KMS_SOURCES
      src/vaapi_decoder.cpp
      src/vaapi_decoder_source.cpp
      src/v4l2_decoder_source_adapter.cpp)
    list(APPEND KMS_LIBS PkgConfig::LIBVA)
  endif()
  if(EGL_FOUND AND GLES_FOUND)
    # GPU rotate/convert fallback (GlRotateSource) for 90/270 on planes that
    # can't rotate; needs drm-cxx's GlScanoutProducer (EGL) plus GLESv2.
    list(APPEND KMS_SOURCES src/gl_rotate_source.cpp)
    list(APPEND KMS_LIBS PkgConfig::EGL PkgConfig::GLES)
  endif()

  add_executable(carlinkit-kms ${KMS_SOURCES})
  target_include_directories(carlinkit-kms PRIVATE ${DRM_CXX_DIR}/examples/common)
  target_link_libraries(carlinkit-kms PRIVATE ${KMS_LIBS})
  if(CARLINKIT_SOFTWARE_ONLY)
    target_compile_definitions(carlinkit-kms PRIVATE CARLINKIT_SOFTWARE_ONLY)
  endif()
  if(EGL_FOUND AND GLES_FOUND)
    target_compile_definitions(carlinkit-kms PRIVATE CARLINKIT_GL)
  endif()
  target_compile_options(carlinkit-kms PRIVATE -Wall -Wextra)
else()
  message(STATUS "carlinkit-kms skipped (need libav, libswscale, libdrm, the drm-cxx submodule at ${DRM_CXX_DIR} — run 'git submodule update --init --recursive' — and libva unless CARLINKIT_SOFTWARE_ONLY)")
endif()
