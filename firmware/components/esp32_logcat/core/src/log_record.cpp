#include "logcat/log_record.hpp"

namespace logcat {

namespace {

// Copies at most N - 1 chars and always terminates dst. Never reads src past the first N
// chars, so unterminated garbage cannot run away. Returns true if src did not fit.
// The capacity comes from the array type, so it cannot drift from the buffer it describes.
template <size_t N>
bool copy_bounded(char (&dst)[N], const char* src) {
    static_assert(N > 0, "destination needs room for the terminator");
    size_t length = 0;
    if (src != nullptr) {
        while (length + 1 < N && src[length] != '\0') {
            dst[length] = src[length];
            ++length;
        }
    }
    dst[length] = '\0';
    return src != nullptr && src[length] != '\0';
}

}  // namespace

void LogRecord::set_tag(const char* text) {
    copy_bounded(tag, text);
}

void LogRecord::set_message(const char* text) {
    truncated = copy_bounded(msg, text);
}

}  // namespace logcat
