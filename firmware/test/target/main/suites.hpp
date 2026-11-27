#pragma once

// The suites of the on-board test app (#93). Each one was a PlatformIO test with its own
// app_main and setUp; here test_main.cpp runs them in turn, and Unity's setUp() calls the
// set-up of the suite that is running.

void littlefs_set_up();
void run_littlefs_tests();  // formats the `logs` partition

void nvs_set_up();
void run_nvs_tests();  // own NVS namespace (`logcat_test`): the real boot counter is untouched
