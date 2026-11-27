# The demo app (ESP-IDF project): build, flash and monitor with idf.py from this folder.
#   idf.py build
#   idf.py -p COMx flash
# Timing variant (#13/#14), its own build folder and sdkconfig:
#   idf.py -B build-timing -D SDKCONFIG=build-timing/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.timing" build
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-logcat-demo)
