#include "logcat/log_record.hpp"

namespace logcat {

namespace {

// Copies at most capacity - 1 chars and always terminates dst. Never reads src past the
// first capacity chars, so unterminated garbage cannot run away. Returns true if src did
// not fit.
bool copy_bounded(char* dst, size_t capacity, const char* src) {
    size_t length = 0;
    if (src != nullptr) {
        while (length + 1 < capacity && src[length] != '\0') {
            dst[length] = src[length];
            ++length;
        }
    }
    dst[length] = '\0';
    return src != nullptr && src[length] != '\0';
}

}  // namespace

void LogRecord::set_tag(const char* text) {
    copy_bounded(tag, kTagCapacity, text);
}

void LogRecord::set_message(const char* text) {
    truncated = copy_bounded(msg, kMsgCapacity, text);
}

}  // namespace logcat
