#pragma once

#include <cstdint>

#include "logcat/boot_counter.hpp"
#include "nvs.h"

namespace logcat {

// The boot counter in NVS (#25): namespace `logcat`, key `boot`, a u32. One 4-byte write per
// boot, spread over the 24 KB `nvs` partition by NVS's own wear leveling: negligible wear.
//
// begin() initializes NVS for the whole firmware. A partition that is full or from another NVS
// version is erased and initialized again (erased() says so): the counter restarts at 1.
// Boot only, before the log hook: NVS calls may block and write flash.
class NvsBootStore final : public BootStore {
public:
    explicit NvsBootStore(const char* name_space = "logcat") : namespace_(name_space) {}
    ~NvsBootStore();  // closes the handle (the firmware's instance is static: never)
    NvsBootStore(const NvsBootStore&) = delete;
    NvsBootStore& operator=(const NvsBootStore&) = delete;

    bool begin();
    bool erased() const { return erased_; }

    BootLoad load(uint32_t& value) override;
    bool save(uint32_t value) override;

private:
    const char* namespace_;
    nvs_handle_t handle_ = 0;
    bool open_ = false;
    bool erased_ = false;
};

}  // namespace logcat
