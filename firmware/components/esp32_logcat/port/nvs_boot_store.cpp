#include "port/nvs_boot_store.hpp"

#include "esp_err.h"
#include "nvs_flash.h"

namespace logcat {

namespace {

constexpr char kKey[] = "boot";

}  // namespace

NvsBootStore::~NvsBootStore() {
    if (open_) {
        nvs_close(handle_);
    }
}

bool NvsBootStore::begin() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // The documented recovery: the partition cannot be used as it is. Everything in it goes,
        // the boot counter included (the boot line says so).
        erased_ = nvs_flash_erase() == ESP_OK;
        err = erased_ ? nvs_flash_init() : err;
    }
    open_ = err == ESP_OK && nvs_open(namespace_, NVS_READWRITE, &handle_) == ESP_OK;
    return open_;
}

BootLoad NvsBootStore::load(uint32_t& value) {
    if (!open_) {
        return BootLoad::Failed;
    }
    const esp_err_t err = nvs_get_u32(handle_, kKey, &value);
    if (err == ESP_OK) {
        return BootLoad::Found;
    }
    return err == ESP_ERR_NVS_NOT_FOUND ? BootLoad::Missing : BootLoad::Failed;
}

bool NvsBootStore::save(uint32_t value) {
    // Committed before the first line goes out: a crash right after boot still counts this boot.
    return open_ && nvs_set_u32(handle_, kKey, value) == ESP_OK && nvs_commit(handle_) == ESP_OK;
}

}  // namespace logcat
