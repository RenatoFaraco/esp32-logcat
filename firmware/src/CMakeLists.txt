idf_component_register(SRCS "main.cpp"
                            "adapters/console_sink.cpp"
                            "adapters/idf_log_hook.cpp"
                            "adapters/log_queue.cpp"
                            "adapters/usb_jtag_sink.cpp"
                            "adapters/writer_task.cpp"
                       INCLUDE_DIRS ".")
