# esp32-logcat as an ESP-IDF component (ADR-013): the platform-independent core (core/, no
# ESP-IDF include, ADR-003) and its ESP-IDF adapters (port/). The public C API comes in #94;
# until then the demo wires the adapters itself, so port/ headers are public too.
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
    INCLUDE_DIRS "core/include" "."
    # What the adapters include (port/*.hpp are public, see above). LittleFS comes from
    # idf_component.yml: the component manager adds it to the requirements.
    REQUIRES
        freertos
        log
        esp_system
        esp_timer
        esp_hw_support
        esp_partition
        esp_app_format
        esp_rom
        spi_flash
        nvs_flash
        esp_driver_uart
        esp_driver_usb_serial_jtag
)
