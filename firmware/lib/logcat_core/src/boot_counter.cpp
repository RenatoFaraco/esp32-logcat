#include "logcat/boot_counter.hpp"

namespace logcat {

BootInfo next_boot(BootStore& store) {
    BootInfo info;
    uint32_t previous = 0;
    switch (store.load(previous)) {
        case BootLoad::Failed:
            return info;  // unknown; the stored value is left for the next boot to read
        case BootLoad::Missing:
            previous = 0;
            info.first = true;
            break;
        case BootLoad::Found:
            break;
    }
    info.boot = previous < kMaxBoot ? previous + 1 : 1;
    info.saved = store.save(info.boot);
    return info;
}

}  // namespace logcat
