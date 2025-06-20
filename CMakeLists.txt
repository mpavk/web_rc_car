cmake_minimum_required(VERSION 3.10)
project(webrccar VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_BUILD_TYPE Debug)

# Використовуємо C++20 і unstable GStreamer API
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_definitions(GST_USE_UNSTABLE_API)

# Вимагаємо pkg-config
find_package(PkgConfig REQUIRED)

# GStreamer core, WebRTC і SDP модулі
pkg_check_modules(GST REQUIRED
  gstreamer-1.0
  gstreamer-webrtc-1.0
  gstreamer-sdp-1.0
)

# GLib для базових можливостей
pkg_check_modules(GLIB REQUIRED
  glib-2.0
  gobject-2.0
  gio-2.0
)

# libsoup 3.x для WebSocket signaling
pkg_check_modules(SOUP REQUIRED
  libsoup-3.0
)

# JSON-GLib для роботи з JSON
pkg_check_modules(JSONGLIB REQUIRED
  json-glib-1.0
)

# libgpiod для роботи з GPIO
pkg_check_modules(LIBGPIOD REQUIRED
  libgpiod
)

# pigpio для роботи з GPIO
# pkg_check_modules(PIGPIO REQUIRED pigpio)

# Список модулів-джерел
set(SOURCES
  main.cpp
  gpio_control.cpp
  pwm_control.cpp
  webrtc_pipeline.cpp
)

# Виконуваний файл
add_executable(webrccar ${SOURCES})

# Додаткові include папки
target_include_directories(webrccar PRIVATE
  ${GST_INCLUDE_DIRS}
  ${GLIB_INCLUDE_DIRS}
  ${SOUP_INCLUDE_DIRS}
  ${JSONGLIB_INCLUDE_DIRS}
  ${LIBGPIOD_INCLUDE_DIRS}
)

# Лінкування з бібліотеками
target_link_libraries(webrccar PRIVATE
  ${GST_LIBRARIES}
  ${GLIB_LIBRARIES}
  ${SOUP_LIBRARIES}
  ${JSONGLIB_LIBRARIES}
  ${LIBGPIOD_LIBRARIES}
  pigpio
)

# Enable testing and packaging
include(CTest)
enable_testing()
set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)
