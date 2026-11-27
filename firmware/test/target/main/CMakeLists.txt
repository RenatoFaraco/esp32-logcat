# The adapters are the component's internals (only esp32_logcat.h is public): this white-box
# test app reaches them through the component's private include directories.
set(LOGCAT_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../components/esp32_logcat")
idf_component_register(SRCS "test_main.cpp"
                            "littlefs_tests.cpp"
                            "nvs_tests.cpp"
                       INCLUDE_DIRS "." "${LOGCAT_DIR}/core/include" "${LOGCAT_DIR}"
                       REQUIRES unity esp32_logcat nvs_flash freertos)
