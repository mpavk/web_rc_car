cmake_minimum_required(VERSION 3.10)
project(webrccar VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_BUILD_TYPE Debug)

# Use C++20 and unstable GStreamer API
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_definitions(-DGST_USE_UNSTABLE_API)

# Require pkg-config
find_package(PkgConfig REQUIRED)

# GStreamer core, WebRTC and SDP modules
pkg_check_modules(GST REQUIRED
  gstreamer-1.0
  gstreamer-webrtc-1.0
  gstreamer-sdp-1.0
)

# GLib for core facilities
pkg_check_modules(GLIB REQUIRED
  glib-2.0
  gobject-2.0
)

# Use libsoup 3.x for WebSocket signalling (must match GStreamer plugins)
pkg_check_modules(SOUP REQUIRED
  libsoup-3.0
)

# JSON-GLib for JSON handling
pkg_check_modules(JSONGLIB REQUIRED
  json-glib-1.0
)

# Include directories
include_directories(
  ${GST_INCLUDE_DIRS}
  ${GLIB_INCLUDE_DIRS}
  ${SOUP_INCLUDE_DIRS}
  ${JSONGLIB_INCLUDE_DIRS}
)

# Link directories
link_directories(
  ${GST_LIBRARY_DIRS}
  ${GLIB_LIBRARY_DIRS}
  ${SOUP_LIBRARY_DIRS}
  ${JSONGLIB_LIBRARY_DIRS}
)

# Executable target
add_executable(webrccar main.cpp)

target_link_libraries(webrccar
  ${GST_LIBRARIES} ${GST_EXTERNAL_LIBRARIES}
  ${GLIB_LIBRARIES}
  ${SOUP_LIBRARIES}
  ${JSONGLIB_LIBRARIES}
)

# Enable CTest and packaging
include(CTest)
enable_testing()
set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
