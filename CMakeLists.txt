cmake_minimum_required(VERSION 3.10.0)
project(webrccar VERSION 0.1.0 LANGUAGES C CXX)

# Вимагати C++20
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Знайти GStreamer через pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED
  gstreamer-1.0
  gstreamer-app-1.0
)

# Додати include-директорії та лінк-директорії GStreamer
include_directories(${GST_INCLUDE_DIRS})
link_directories(${GST_LIBRARY_DIRS})

# Наш виконуваний файл
add_executable(webrccar main.cpp)

# Лінкуємо з GStreamer
target_link_libraries(webrccar ${GST_LIBRARIES})

# Тести та пакування
include(CTest)
enable_testing()

set(CPACK_PROJECT_NAME ${PROJECT_NAME})
set(CPACK_PROJECT_VERSION ${PROJECT_VERSION})
include(CPack)