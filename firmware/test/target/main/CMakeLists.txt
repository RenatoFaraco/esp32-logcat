idf_component_register(SRCS "test_main.cpp"
                            "littlefs_tests.cpp"
                            "nvs_tests.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES unity esp32_logcat nvs_flash)
