# esp32-logcat as an ESP-IDF component (ADR-013): the platform-independent core (core/, no
# ESP-IDF include, ADR-003) and its ESP-IDF adapters (port/), behind a C API
# (include/esp32_logcat.h, #94). Only that header is public.
idf_component_register(
    SRCS
        "core/src/boot_counter.cpp"
        "core/src/command_parser.cpp"
        "core/src/console.cpp"
        "core/src/file_line_reader.cpp"
        "core/src/flush_policy.cpp"
        "core/src/idf_text.cpp"
        "core/src/line_formatter.cpp"
        "core/src/line_parser.cpp"
        "core/src/log_record.cpp"
        "core/src/reply_formatter.cpp"
        "core/src/reply_server.cpp"
        "core/src/rotating_store.cpp"
        "core/src/sequencer.cpp"
        "core/src/tag_level_filter.cpp"
        "core/src/writer.cpp"
        "port/console_sink.cpp"
        "port/esp32_logcat.cpp"
        "port/console_task.cpp"
        "port/flash_store.cpp"
        "port/idf_log_hook.cpp"
        "port/littlefs_file_system.cpp"
        "port/log_queue.cpp"
        "port/nvs_boot_store.cpp"
        "port/reply_queue.cpp"
        "port/reset_reason.cpp"
        "port/usb_jtag_sink.cpp"
        "port/writer_task.cpp"
    INCLUDE_DIRS "include"
    # Internal: the core ("logcat/...") and the adapters ("port/..."). The on-board test app
    # adds them itself: it tests the adapters (firmware/test/target).
    PRIV_INCLUDE_DIRS "core/include" "."
    # Public: what esp32_logcat.h includes (esp_log_level.h; esp_err.h is common).
    REQUIRES
        log
    # Private: everything the core and the adapters use. LittleFS comes from idf_component.yml
    # (require: private).
    PRIV_REQUIRES
        freertos
        esp_system
        esp_timer
        nvs_flash
        esp_hw_support
        esp_partition
        esp_app_format
        esp_rom
        spi_flash
        esp_driver_uart
        esp_driver_usb_serial_jtag
)

# ADR-005: no exceptions and no RTTI in this component, whatever the consuming project's
# sdkconfig says (CONFIG_COMPILER_CXX_EXCEPTIONS/RTTI apply to the whole build otherwise).
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>)
