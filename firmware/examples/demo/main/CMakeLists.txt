# The demo's main component. With no REQUIRES, `main` sees every component of the project,
# esp32_logcat and logcat_demo included.
idf_component_register(SRCS "main.cpp"
                            "app/critical_task.cpp"
                            "app/timing_sweep.cpp"
                       INCLUDE_DIRS ".")
