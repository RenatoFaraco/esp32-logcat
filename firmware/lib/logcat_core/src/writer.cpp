#include "logcat/writer.hpp"

#include "logcat/line_formatter.hpp"

namespace logcat {

namespace {

// Appends `text` to `out` at `pos`, never past `capacity - 1`; returns the new position.
size_t append(char* out, size_t capacity, size_t pos, const char* text) {
    while (*text != '\0' && pos + 1 < capacity) {
        out[pos++] = *text++;
    }
    out[pos] = '\0';
    return pos;
}

size_t append_uint(char* out, size_t capacity, size_t pos, uint32_t value) {
    char digits[10];  // uint32_t has at most 10 decimal digits
    size_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count > 0 && pos + 1 < capacity) {
        out[pos++] = digits[--count];
    }
    out[pos] = '\0';
    return pos;
}

}  // namespace

bool Writer::add_sink(Sink& sink) {
    if (sink_count_ >= kMaxSinks) {
        return false;
    }
    sinks_[sink_count_++] = SinkState{&sink, 0, true};
    return true;
}

void Writer::write(const LogRecord& record) {
    send(format_line(record, line_));
}

void Writer::send(size_t length) {
    for (size_t i = 0; i < sink_count_; ++i) {
        SinkState& state = sinks_[i];
        state.last_ok = state.sink->write(line_, length);
        if (!state.last_ok) {
            ++state.unsent;
        }
    }
}

void Writer::idle() {
    const uint32_t dropped = sequencer_.take_dropped();
    if (dropped != 0) {
        report(LogLevel::Warn, dropped, "dropped ", " lines: log queue full");
    }
    for (size_t i = 0; i < sink_count_; ++i) {
        SinkState& state = sinks_[i];
        if (state.unsent == 0 || !state.last_ok) {
            continue;
        }
        const uint32_t count = state.unsent;
        state.unsent = 0;
        // The report goes to every sink, like any line: the others would otherwise see a gap
        // in seq. If this sink refuses it again, send() counts it as unsent.
        char after[48];
        size_t pos = append(after, sizeof(after), 0, " lines not sent to ");
        pos = append(after, sizeof(after), pos, state.sink->name());
        append(after, sizeof(after), pos, ": no reader");
        report(LogLevel::Warn, count, "", after);
    }
}

void Writer::report(LogLevel level, uint32_t count, const char* text_before,
                    const char* text_after) {
    LogRecord record{};
    record.boot = boot_;
    record.seq = sequencer_.next();
    record.uptime_ms = clock_.uptime_ms();
    record.level = level;
    record.set_tag(kWriterTag);
    size_t pos = append(record.msg, kMsgCapacity, 0, text_before);
    pos = append_uint(record.msg, kMsgCapacity, pos, count);
    append(record.msg, kMsgCapacity, pos, text_after);
    write(record);
    // A sink that refused the report also missed what it explained: keep that count (send()
    // already counted the report line itself), so its next report covers the whole hole.
    for (size_t i = 0; i < sink_count_; ++i) {
        if (!sinks_[i].last_ok) {
            sinks_[i].unsent += count;
        }
    }
}

uint32_t Writer::unsent(size_t sink_index) const {
    return sink_index < sink_count_ ? sinks_[sink_index].unsent : 0;
}

}  // namespace logcat
