#pragma once

namespace logcat {

// The lock that serializes writers of the TagLevelFilter (tag_level_filter.hpp): the console
// task and esp32_logcat_set_level(). A static FreeRTOS mutex, created by esp32_logcat_init()
// before the console starts. Readers (every log call) never take it.
void lock_tag_filter();
void unlock_tag_filter();

}  // namespace logcat
