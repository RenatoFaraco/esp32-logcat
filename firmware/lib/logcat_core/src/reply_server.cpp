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
        output_.try_write_reply(line_, length);
    }
}

void ReplyServer::write_line(size_t length) {
    if (length == 0 || !output_.write_reply(line_, length)) {
        count_failure();
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
    if (message.kind == ReplyKind::Dump) {
        begin_dump(message.id);
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
    mode_ = Mode::Tail;
    const size_t length = format_dump_begin(id_, kBlockName, tail_.total_bytes(), line_);
    if (length == 0 || !output_.write_reply(line_, length)) {
        fail_block();
    }
}

void ReplyServer::step() {
    switch (mode_) {
        case Mode::Tail:
            step_tail();
            return;
        case Mode::Dump:
            step_dump();
            return;
        case Mode::Idle:
            return;
    }
}

void ReplyServer::step_tail() {
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
    write_line(format_reply_ok(id_, line_));
    end_block();
}

void ReplyServer::begin_dump(const char* id) {
    std::memcpy(id_, id, sizeof(id_));
    if (archive_ == nullptr) {
        write_line(format_reply_error(id_, "unsupported", "no flash storage", line_));
        return;
    }
    file_count_ = 0;
    const bool synced = archive_->begin_dump(files_, file_count_);
    if (!synced) {
        archive_->end_dump();
        write_line(
            format_reply_error(id_, "failed", "sync failed: the dump would miss lines", line_));
        return;
    }
    // The steps send the blocks: #BEGIN of the first file on the next one.
    gate_.set_closed(true);
    file_ = 0;
    offset_ = 0;
    in_block_ = false;
    mode_ = Mode::Dump;
}

void ReplyServer::step_dump() {
    if (file_ >= file_count_) {
        write_line(format_reply_ok(id_, line_));  // one terminator for every block
        end_block();
        return;
    }
    const ArchiveFile& file = files_[file_];
    if (!in_block_) {
        const size_t length = format_dump_begin(id_, file.name, file.size, line_);
        if (length == 0 || !output_.write_reply(line_, length)) {
            fail_block();
            return;
        }
        in_block_ = true;
        offset_ = 0;
        crc_ = 0;
        return;
    }
    if (offset_ < file.size) {
        const uint32_t left = file.size - offset_;
        const size_t wanted = left < kDumpChunk ? left : kDumpChunk;
        if (archive_->read(file.name, offset_, chunk_, wanted) != wanted) {
            // The store removed (or renamed) the file since the dump began: the rotation goes on
            // rather than refusing lines, and the blocks already closed stay valid (§3).
            write_line(format_reply_error(id_, "failed", "log rotated during dump", line_));
            end_block();
            return;
        }
        crc_ = crc32_update(crc_, chunk_, wanted);
        if (!output_.write_reply(chunk_, wanted)) {
            fail_block();
            return;
        }
        offset_ += static_cast<uint32_t>(wanted);
        return;
    }
    const size_t length = format_dump_end(id_, crc_, line_);
    if (length == 0 || !output_.write_reply(line_, length)) {
        fail_block();
        return;
    }
    in_block_ = false;
    ++file_;
}

void ReplyServer::fail_block() {
    count_failure();
    send_timeout(id_);  // "#!" without "#END": the host discards the block
    end_block();
}

void ReplyServer::end_block() {
    if (mode_ == Mode::Tail) {
        tail_.unfreeze();
    } else if (mode_ == Mode::Dump) {
        archive_->end_dump();
    }
    mode_ = Mode::Idle;
    gate_.set_closed(false);
}

}  // namespace logcat
