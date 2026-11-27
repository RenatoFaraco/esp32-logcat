#pragma once

#include <cstddef>

namespace logcat {

// Where formatted lines go (USB, UART console, flash in M4). Implemented by adapters; the core
// only sees this interface (ADR-003).
//
// Called only from the writer task. write() must not block for long (zero timeout on the USB
// driver) and returns false when the line was not taken as a whole: the writer counts it and
// reports it once the sink accepts lines again.
class Sink {
public:
    virtual bool write(const char* line, size_t length) = 0;
    virtual const char* name() const = 0;  // short, printable, no spaces: "usb", "console"

protected:
    ~Sink() = default;  // never deleted through the interface: sinks are statics
};

}  // namespace logcat
