// The on-board tests (#93): the ESP-IDF adapters on the real chip, flash and NVS, as one test
// app. Output goes to the primary console (UART0, the CH340K bridge):
//
//   cd firmware/test/target
//   idf.py -p COMx flash monitor
//
// The run ends with Unity's summary: "N Tests 0 Failures 0 Ignored" and "OK".
//
// WARNING: the LittleFS suite formats the `logs` partition. The logs kept on the board are erased.

#include <unity.h>

#include <cstdio>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "suites.hpp"

namespace {

struct Suite {
    const char* name;
    void (*set_up)();
    void (*run)();
};

// In this order: the LittleFS suite's last test remounts the partition.
constexpr Suite kSuites[] = {
    {"littlefs", littlefs_set_up, run_littlefs_tests},
    {"nvs", nvs_set_up, run_nvs_tests},
};

const Suite* current = nullptr;

}  // namespace

// Strong definitions: ESP-IDF's unity component only has weak ones (unity_compat.c).
extern "C" void setUp(void) {
    if (current != nullptr) {
        current->set_up();
    }
}
extern "C" void tearDown(void) {}

extern "C" void app_main(void) {
    vTaskDelay(pdMS_TO_TICKS(2000));  // let the host open the port
    UNITY_BEGIN();
    for (const Suite& suite : kSuites) {
        UnityPrint("suite: ");
        UnityPrint(suite.name);
        UNITY_PRINT_EOL();
        current = &suite;
        suite.run();
    }
    current = nullptr;
    // After every suite, the heaviest frames included (the remount test, the NVS suite).
    printf("main task stack left: %u B of %u\n",
           static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
           static_cast<unsigned>(CONFIG_ESP_MAIN_TASK_STACK_SIZE));
    UNITY_END();
}
