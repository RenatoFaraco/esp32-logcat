import json

from esp_logcat.console import (
    Exchange,
    Reply,
    RequestIds,
    block_lines,
    block_parsed,
    reply_json,
)
from esp_logcat.events import Block, Notice, NoticeKind, Received, ReceivedBlock
from esp_logcat.parser import CommandEnd, InvalidLine, LogLine, parse_line


def line(data: bytes) -> Received:
    return Received(parse_line(data), None)


def feed(exchange: Exchange, *items: bytes | Notice | ReceivedBlock, now: float = 0.1):
    """All completed replies, and which inputs were out of context."""
    done, outside = [], []
    for item in items:
        event = line(item) if isinstance(item, bytes) else item
        replies, out_of_context = exchange.observe(event, now)
        done += replies
        if out_of_context:
            outside.append(item)
    return done, outside


def exchange(start: int = 7) -> Exchange:
    return Exchange(RequestIds(start=start))


LOG = b"0000:000001 0000000010 I T: live\n"


# ------------------------------------------------------------------ ids


def test_ids_rotate_and_wrap_after_9999():
    ids = RequestIds(start=9998)
    assert [ids.take(lambda _: False) for _ in range(3)] == ["9998", "9999", "1"]


def test_ids_start_at_a_random_place():
    assert RequestIds(rng=lambda low, high: 4242).take(lambda _: False) == "4242"


def test_busy_ids_are_skipped():
    ids = RequestIds(start=5)
    assert ids.take(lambda candidate: candidate in {"5", "6"}) == "7"


def test_an_id_is_not_reused_within_the_timeout():
    ex = Exchange(RequestIds(start=1))
    first = ex.start("info", now=0.0)
    feed(ex, f"#. {first} ok\n".encode())
    ids = RequestIds(start=1)
    ex._ids = ids  # back to the same id on purpose
    assert ex.start("info", now=1.0) != first  # 1 s later: still within 2 s
    ex._ids = RequestIds(start=1)
    assert ex.start("info", now=5.0) == first  # long after: free again


# ------------------------------------------------------------------ replies


def test_data_lines_and_terminator_make_one_reply():
    ex = exchange()
    ident = ex.start("stats", now=0.0)
    done, outside = feed(
        ex,
        b"#> 7 dropped=42 queue=0/32\n",
        LOG,  # log lines come between reply lines
        b"#> 7 tail_skipped=0\n",
        b"#. 7 ok\n",
        now=0.06,
    )
    assert ident == "7" and outside == []
    [reply] = done
    assert reply.ok and reply.fields == [
        ("dropped", "42"),
        ("queue", "0/32"),
        ("tail_skipped", "0"),
    ]
    assert reply.elapsed == 0.06
    assert ex.pending == []


def test_error_terminator():
    ex = exchange()
    ex.start("log level X D", now=0.0)
    [reply], _ = feed(ex, b"#! 7 full tag table full (16 tags)\n")
    assert not reply.ok and reply.end == CommandEnd("7", "full", "tag table full (16 tags)")
    assert reply.failure == ""


def test_lines_of_another_id_are_out_of_context():
    ex = exchange()
    ex.start("info", now=0.0)
    done, outside = feed(ex, b"#> 8 a=1\n", b"#. 8 ok\n", b"#> 7 a=2\n", b"#. 7 ok\n")
    assert outside == [b"#> 8 a=1\n", b"#. 8 ok\n"]
    assert done[0].fields == [("a", "2")]


def test_reply_lines_with_nothing_pending_are_out_of_context():
    _, outside = feed(exchange(), b"#. 3 ok\n", b"#> - a=1\n", LOG)
    assert outside == [b"#. 3 ok\n", b"#> - a=1\n"]


def test_first_line_after_connecting_is_out_of_context():
    ex = exchange()
    ex.start("info", now=0.0)
    connected = Notice(NoticeKind.CONNECTED, "connected to COM6")
    done, outside = feed(ex, connected, b"#. 7 ok\n")
    assert outside == [b"#. 7 ok\n"] and done == []
    done, _ = feed(ex, b"#. 7 ok\n")
    assert done[0].ok


def test_a_reply_that_is_the_first_line_is_fine_when_asked_after_connecting():
    ex = exchange()
    feed(ex, Notice(NoticeKind.CONNECTED, "connected to COM6"), now=0.0)
    ex.start("info", now=0.5)  # a quiet device: the reply is the first line
    done, outside = feed(ex, b"#. 7 ok\n", now=0.6)
    assert outside == [] and done[0].ok


def test_begin_and_end_lines_reaching_us_unframed_are_out_of_context():
    ex = exchange()
    ex.start("log tail", now=0.0)
    _, outside = feed(ex, b"#BEGIN 7 ram 0\n", b"#END 7 00000000\n")
    assert len(outside) == 2


def test_no_reply_in_time():
    ex = exchange()
    ex.start("info", now=0.0)
    assert ex.expire(now=1.9) == []
    [reply] = ex.expire(now=2.0)
    assert not reply.ok and reply.failure == "no reply in 2 s"
    assert ex.pending == []


def test_cancel_when_the_write_failed():
    ex = exchange()
    ident = ex.start("info", now=0.0)
    reply = ex.cancel(ident, "the port is not open", now=0.0)
    assert reply is not None and reply.failure == "the port is not open"
    assert ex.cancel(ident, "again", now=0.0) is None


# ------------------------------------------------------------------ blocks


def good_block(ident: str = "7") -> Block:
    content = LOG + LOG
    return Block(ident, "ram", len(content), content, 0)


def test_block_then_terminator():
    ex = exchange()
    ex.start("log tail", now=0.0)
    done, _ = feed(ex, ReceivedBlock(good_block(), None))
    assert done == []
    [reply], _ = feed(ex, b"#. 7 ok\n")
    assert reply.ok and reply.block == good_block()


def test_damaged_block_ends_the_request_at_once():
    ex = exchange()
    ex.start("log tail", now=0.0)
    bad = Block("7", "ram", 10, b"abc", None, "no #END after the content")
    [reply], _ = feed(ex, ReceivedBlock(bad, None))
    assert not reply.ok and "no #END" in reply.failure


def test_block_for_nobody_is_out_of_context():
    _, outside = feed(exchange(), ReceivedBlock(good_block("9"), None))
    assert len(outside) == 1


def test_bytes_inside_a_block_keep_the_request_alive():
    ex = exchange()
    ex.start("log tail", now=0.0)
    ex.touch("7", now=1.5)
    assert ex.expire(now=3.0) == []
    assert ex.expire(now=3.5)[0].failure == "no reply in 2 s"


# ------------------------------------------------------------------ restarts


def test_reboot_is_done_when_the_device_restarts():
    ex = exchange()
    ex.start("reboot", now=0.0)
    done, _ = feed(ex, b"#. 7 ok\n")
    assert done == []  # acknowledged, now waiting for the restart
    assert ex.expire(now=5.0) == []  # longer than the reply timeout
    [reply], _ = feed(ex, Notice(NoticeKind.DISCONNECTED, "COM6: connection lost"))
    assert reply.ok and reply.restarted


def test_reboot_without_restart_fails():
    ex = exchange()
    ex.start("reboot", now=0.0)
    feed(ex, b"#. 7 ok\n", now=0.1)
    [reply] = ex.expire(now=10.2)
    assert not reply.ok and reply.failure == "no restart seen in 10 s"


def test_crash_has_no_reply_and_succeeds_on_restart():
    ex = exchange()
    ex.start("crash panic", now=0.0)
    assert ex.expire(now=3.0) == []  # no reply expected: the restart window applies
    [reply], _ = feed(ex, Notice(NoticeKind.RESTART, "restart"))
    assert reply.ok and reply.restarted and reply.end is None


def test_crash_without_restart_fails():
    ex = exchange()
    ex.start("crash wdt", now=0.0)
    [reply] = ex.expire(now=10.0)
    assert not reply.ok and "no restart" in reply.failure


def test_restart_loses_any_other_pending_reply():
    ex = exchange()
    ex.start("info", now=0.0)
    [reply], _ = feed(ex, Notice(NoticeKind.RESTART, "restart"))
    assert not reply.ok and reply.failure == "no reply: the device restarted"


def test_port_closing_loses_a_pending_reply():
    ex = exchange()
    ex.start("stats", now=0.0)
    [reply], _ = feed(ex, Notice(NoticeKind.DISCONNECTED, "lost"))
    assert reply.failure == "no reply: the port closed"


# ------------------------------------------------------------------ JSON and block lines


def test_json_numbers_strings_and_repeated_keys():
    reply = Reply(
        "7", "stats", [("dropped", "42"), ("queue", "0/32"), ("k", "1"), ("k", "2"), ("k", "x")]
    )
    reply.end = CommandEnd("7", "ok")
    out = reply_json(reply)
    assert out["id"] == 7 and out["ok"] is True
    assert out["fields"] == {"dropped": 42, "queue": "0/32", "k": [1, 2, "x"]}
    json.dumps(out)  # serializable


def test_json_of_an_error_and_of_a_failure():
    reply = Reply("-", "log level X D", end=CommandEnd("-", "full", "tag table full (16 tags)"))
    out = reply_json(reply)
    assert out["id"] == "-" and out["ok"] is False
    assert out["code"] == "full" and out["text"] == "tag table full (16 tags)"
    assert "fields" not in out and "error" not in out
    lost = reply_json(Reply("7", "info", failure="no reply in 2 s"))
    assert lost["error"] == "no reply in 2 s" and lost["ok"] is False


def test_json_of_a_block_and_of_a_restart():
    reply = Reply("7", "log tail", end=CommandEnd("7", "ok"), block=good_block())
    out = reply_json(reply)
    assert out["block"] == {
        "name": "ram",
        "bytes": len(LOG) * 2,
        "crc_ok": True,
        "lines": [LOG[:-1].decode()] * 2,
    }
    assert reply_json(Reply("7", "crash panic", restarted=True))["restarted"] is True


def test_block_lines_escape_bytes_outside_ascii():
    block = Block("7", "ram", 6, b"a\xff\nb\x7f\n", 0)
    assert block_lines(block) == ["a\\xff", "b\x7f"]


def test_block_lines_go_through_the_log_parser():
    block = Block("7", "ram", 0, LOG + b"0000:000002 0000000011 W T: cut\x7f\n" + b"\xff\xff", 0)
    parsed = block_parsed(block)
    assert isinstance(parsed[0], LogLine)
    assert isinstance(parsed[1], InvalidLine) and parsed[1].reason == "torn"
    assert isinstance(parsed[2], InvalidLine)  # erased flash with no newline
