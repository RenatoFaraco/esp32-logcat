import zlib
from pathlib import Path

import pytest

from esp_logcat.events import Block
from esp_logcat.framing import Framer

CONFORMANCE = Path(__file__).parents[2] / "conformance"

# A block's content as flash may hold it: a good line, a torn one closed with DEL, erased
# flash, a NUL and a run without a newline.
CONTENT = (
    b"0042:000001 0000000010 I APP: ok\n"
    b"0042:000002 0000000011 W APP: cut\x7f\n" + b"\xff" * 4 + b"\x00" + b"x" * 40
)


def block_bytes(ident: str, content: bytes, crc: int | None = None) -> bytes:
    crc = zlib.crc32(content) if crc is None else crc
    return (
        f"#BEGIN {ident} ram {len(content)}\n".encode()
        + content
        + f"#END {ident} {crc:08x}\n".encode()
    )


def frame(chunks: list[bytes], pending: set[str] = frozenset({"4"})) -> list[bytes | Block]:
    framer = Framer(lambda ident: ident in pending)
    out: list[bytes | Block] = []
    for chunk in chunks:
        out += framer.feed(chunk)
    return out + framer.flush()


def one_byte_at_a_time(data: bytes) -> list[bytes]:
    return [data[i : i + 1] for i in range(len(data))]


LOG = b"0000:000001 0000000010 I T: live\n"
STREAM = LOG + block_bytes("4", CONTENT) + b"#. 4 ok\n" + LOG


@pytest.mark.parametrize("chunks", [[STREAM], one_byte_at_a_time(STREAM)], ids=["whole", "bytes"])
def test_block_is_read_by_byte_count_whatever_the_chunks(chunks):
    out = frame(chunks)
    assert out[0] == LOG
    block = out[1]
    assert isinstance(block, Block)
    assert block.ok and block.content == CONTENT and block.id == "4" and block.name == "ram"
    assert block.crc32 == zlib.crc32(CONTENT)
    assert out[2:] == [b"#. 4 ok\n", LOG]


def test_content_is_kept_byte_for_byte_even_with_crlf_and_newlines_inside():
    content = b"a\r\nb\n\n\rc"
    out = frame([block_bytes("4", content)])
    assert out[0].content == content  # the splitter would have turned \r\n into \n


def test_empty_block():
    out = frame([b"#BEGIN 4 ram 0\n#END 4 00000000\n#. 4 ok\n"])
    assert out[0] == Block("4", "ram", 0, b"", 0)
    assert out[1] == b"#. 4 ok\n"


def test_wrong_crc_is_a_bad_block():
    out = frame([block_bytes("4", CONTENT, crc=0x12345678) + b"#. 4 ok\n"])
    assert not out[0].ok and "CRC mismatch" in out[0].problem
    assert out[1] == b"#. 4 ok\n"


def test_uppercase_crc_is_accepted():
    data = block_bytes("4", CONTENT).replace(
        f"{zlib.crc32(CONTENT):08x}".encode(), f"{zlib.crc32(CONTENT):08X}".encode()
    )
    assert frame([data])[0].ok


def test_end_with_crlf_is_accepted():
    data = block_bytes("4", CONTENT)[:-1] + b"\r\n"
    assert frame([data])[0].ok


# The device gave up inside the block (wire-format §3): the host reads <bytes> bytes, so the
# "#!" and what follows count as content, and the line after them is not the #END.
def test_error_inside_the_block_is_noticed_at_the_missing_end():
    content = b"0042:000001 0000000010 I APP: ok\n"
    stream = (
        f"#BEGIN 4 ram {len(content) + 10}\n".encode()
        + content
        + b"#! 4 timeout host not reading\n"
        + LOG
    )
    out = frame([stream])
    block = out[0]
    assert isinstance(block, Block) and not block.ok and "no #END" in block.problem
    assert block.content == (content + b"#! 4 timeout host not reading\n")[: len(content) + 10]
    assert out[1:] == [b"ut host not reading\n", LOG]  # live data again, not swallowed


def test_end_of_another_id_is_not_this_block():
    out = frame([block_bytes("4", CONTENT).replace(b"#END 4", b"#END 5")])
    assert not out[0].ok
    assert out[1].startswith(b"#END 5")  # shown, not swallowed


def test_begin_without_a_pending_request_is_an_ordinary_line():
    data = block_bytes("9", b"a\nb\n")
    out = frame([data])
    assert out == [
        b"#BEGIN 9 ram 4\n",
        b"a\n",
        b"b\n",
        b"#END 9 " + f"{zlib.crc32(b'a\nb\n'):08x}".encode() + b"\n",
    ]


def test_torn_begin_is_not_a_block():
    out = frame([b"#BEGIN 4 ram 4"])
    assert out == [b"#BEGIN 4 ram 4"]


def test_disconnect_inside_the_block_reports_it_incomplete():
    framer = Framer(lambda ident: True)
    framer.feed(b"#BEGIN 4 ram 100\nabc")
    assert framer.block_id == "4"
    out = framer.flush()
    assert len(out) == 1 and not out[0].ok and "connection lost" in out[0].problem
    assert out[0].content == b"abc"
    assert framer.block_id is None


def test_cancel_drops_the_block_and_resumes_lines():
    framer = Framer(lambda ident: True)
    framer.feed(b"#BEGIN 4 ram 100\nabc")
    framer.cancel_block()
    assert framer.feed(LOG) == [LOG]
    assert framer.block_id is None


def test_end_line_without_newline_that_grows_too_long_is_given_up():
    framer = Framer(lambda ident: True)
    out = framer.feed(b"#BEGIN 4 ram 1\nx" + b"y" * 100)
    assert len(out) == 1 and not out[0].ok


def test_two_blocks_in_a_row():
    stream = block_bytes("4", b"one\n") + block_bytes("4", b"two\n")
    out = frame([stream])
    assert [block.content for block in out] == [b"one\n", b"two\n"]


def test_lines_around_blocks_still_get_crlf_normalized():
    out = frame([b"a\r\n" + block_bytes("4", b"x") + b"b\r\n"])
    assert out[0] == b"a\n" and out[2] == b"b\n"


def test_overflow_count_comes_from_the_splitter():
    framer = Framer(lambda ident: False, max_pending=8)
    framer.feed(b"0123456789abcdef\n")
    assert framer.overflows == 1


def test_shared_crc_cases():
    rows = [
        line.split("\t")
        for line in (CONFORMANCE / "reply_cases.tsv").read_text(encoding="ascii").splitlines()
        if line and not line.startswith("#")
    ]
    crc_rows = [row for row in rows if row[1] == "block_crc"]
    assert crc_rows
    for name, _, content_hex, crc in crc_rows:
        content = bytes.fromhex(content_hex)
        out = frame([block_bytes("4", content, crc=int(crc, 16))])
        assert out[0].ok, name
        assert out[0].content == content, name
