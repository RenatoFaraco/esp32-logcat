# The platform-independent core (components/esp32_logcat/core) tested on the PC: one Unity
# executable per test_* folder, run by ctest. Separate from the ESP-IDF build on purpose: the
# core is compiled here with only its own include path, so an ESP-IDF include cannot build
# (ADR-003). Strict C++17 without exceptions or RTTI (ADR-005).
#
#   cmake --preset native && cmake --build --preset native && ctest --preset native
#   (asan / tsan: Linux only; see CMakePresets.json)
cmake_minimum_required(VERSION 3.22)
project(esp32_logcat_core_tests LANGUAGES C CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(FIRMWARE_DIR "${CMAKE_CURRENT_LIST_DIR}/../..")
set(CORE_DIR "${FIRMWARE_DIR}/components/esp32_logcat/core")
get_filename_component(CONFORMANCE_DIR "${FIRMWARE_DIR}/../conformance" ABSOLUTE)

# Unity, pinned: the tag's tarball and its hash (no git needed).
include(FetchContent)
FetchContent_Declare(unity
    URL https://github.com/ThrowTheSwitch/Unity/archive/refs/tags/v2.6.1.tar.gz
    URL_HASH SHA256=b41a66d45a6b99758fb3202ace6178177014d52fc524bf1f72687d93e9867292
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(unity)

find_package(Threads REQUIRED)  # tests that race the core with std::thread

# Windows (MinGW): link the C++ runtime into each test. Otherwise a test loads the first
# libstdc++-6.dll on PATH, and the ESP-IDF environment puts another one first (exit 0xc0000139).
if(MINGW)
    add_link_options(-static)
endif()

# Our code only (not Unity, which is C): the rules of ADR-005.
function(logcat_strict target)
    set_target_properties(${target} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)  # -std=c++17, checked by test_toolchain
    target_compile_options(${target} PRIVATE
        -Wall -Wextra -Werror
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
endfunction()

file(GLOB CORE_SOURCES CONFIGURE_DEPENDS "${CORE_DIR}/src/*.cpp")
add_library(logcat_core STATIC ${CORE_SOURCES})
target_include_directories(logcat_core PUBLIC "${CORE_DIR}/include")
logcat_strict(logcat_core)

# The demo's latency statistics (plain C++), for test_latency_stats.
set(DEMO_LIB_DIR "${FIRMWARE_DIR}/examples/demo/components/logcat_demo")
add_library(logcat_demo STATIC "${DEMO_LIB_DIR}/src/latency_stats.cpp")
target_include_directories(logcat_demo PUBLIC "${DEMO_LIB_DIR}/include")
logcat_strict(logcat_demo)

enable_testing()
file(GLOB TEST_DIRS LIST_DIRECTORIES true CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/test_*")
foreach(dir IN LISTS TEST_DIRS)
    get_filename_component(name "${dir}" NAME)
    add_executable(${name} "${dir}/test_main.cpp")
    target_link_libraries(${name} PRIVATE logcat_core logcat_demo unity Threads::Threads)
    # The shared wire cases (conformance/*.tsv), wherever ctest runs from.
    target_compile_definitions(${name} PRIVATE LOGCAT_CONFORMANCE_DIR="${CONFORMANCE_DIR}")
    logcat_strict(${name})
    add_test(NAME ${name} COMMAND ${name})
endforeach()
