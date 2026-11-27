# The on-board test app (#93): the ESP-IDF adapters on the real chip, as one Unity app.
#   idf.py -p COMx flash monitor     (UART0, the CH340K bridge: Unity's output)
# Configuration: the demo's sdkconfig.defaults (what the component needs), then this folder's.
# A generated sdkconfig keeps old values: after changing either defaults file, delete
# sdkconfig (or idf.py fullclean) so they apply.
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
set(SDKCONFIG_DEFAULTS
    "${CMAKE_CURRENT_LIST_DIR}/../../examples/demo/sdkconfig.defaults"
    "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-logcat-target-tests)
