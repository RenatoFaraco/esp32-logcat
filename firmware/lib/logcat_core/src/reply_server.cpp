#include "logcat/reply_server.hpp"

#include <cstring>

#include "logcat/reply_formatter.hpp"

namespace logcat {

namespace {

constexpr char kBlockName[] = "ram";

}  // namespace

void ReplyServer::count_failure() {
    // Only this task writes: a load and a store, no emulated read-modify-write on the C3.
    failures_.store(failures_.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

void ReplyServer::send_timeout(const char* id) {
    const size_t length = format_reply_error(id, "timeout", "host not reading", line_);
    if (length != 0) {
        output_.write_reply(line_, length);
    }
}

void ReplyServer::handle(const ReplyMessage& message) {
    if (aborted_) {
        if (std::strcmp(message.id, aborted_id_) == 0) {
            // The rest of a reply that already failed: its terminator closes the matter.
            aborted_ = message.kind == ReplyKind::Data;
            return;
        }
        aborted_ = false;
    }
    if (message.kind == ReplyKind::Tail) {
        begin_tail(message.id);
        return;
    }
    if (output_.write_reply(message.line, message.length)) {
        return;
    }
    count_failure();
    send_timeout(message.id);
    if (message.kind == ReplyKind::Data) {
        aborted_ = true;
        std::memcpy(aborted_id_, message.id, sizeof(aborted_id_));
    }
}

void ReplyServer::begin_tail(const char* id) {
    std::memcpy(id_, id, sizeof(id_));
    tail_.freeze();
    gate_.set_closed(true);
    next_ = 0;
    crc_ = 0;
    open_ = true;
    const size_t length = format_dump_begin(id_, kBlockName, tail_.total_bytes(), line_);
    if (length == 0 || !output_.write_reply(line_, length)) {
        fail_block();
    }
}

void ReplyServer::step() {
    if (!open_) {
        return;
    }
    const TailLine* item = tail_.at(next_);
    if (item != nullptr) {
        ++next_;
        crc_ = crc32_update(crc_, item->text, item->length);
        if (!output_.write_reply(item->text, item->length)) {
            fail_block();
        }
        return;
    }
    size_t length = format_dump_end(id_, crc_, line_);
    if (length == 0 || !output_.write_reply(line_, length)) {
        fail_block();
        return;
    }
    length = format_reply_ok(id_, line_);
    if (length == 0 || !output_.write_reply(line_, length)) {
        count_failure();
    }
    end_block();
}

void ReplyServer::fail_block() {
    count_failure();
    send_timeout(id_);  // "#!" without "#END": the host discards the block
    end_block();
}

void ReplyServer::end_block() {
    open_ = false;
    gate_.set_closed(false);
    tail_.unfreeze();
}

}  // namespace logcat
