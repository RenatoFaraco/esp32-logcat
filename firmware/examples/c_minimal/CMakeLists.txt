# The smallest project using esp32-logcat, in C: what to copy into your own project.
#   idf.py build && idf.py -p COMx flash
# In your project, add the component instead of this path (EXTRA_COMPONENT_DIRS, or later the
# IDF Component Registry).
cmake_minimum_required(VERSION 3.22)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-logcat-c-minimal)
