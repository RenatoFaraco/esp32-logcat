"""The shell against a fake device that answers what is written to it."""

import io
import json
import queue
import zlib
from collections.abc import Iterator

import pytest
from rich.console import Console

from esp_logcat import cli
from esp_logcat import shell as shell_module
from esp_logcat.console import Exchange, RequestIds
from esp_logcat.events import Notice, NoticeKind
from esp_logcat.filters import LogFilter
from esp_logcat.shell import Shell, print_reply, run_lines, run_one

LOG_I = b"0000:000001 0000000010 I APP: hello\n"
LOG_W = b"0000:000002 0000000011 W APP: careful\n"
TAIL = LOG_I + LOG_W


def tail_block(ident: str) -> bytes:
    return (
        f"#BEGIN {ident} ram {len(TAIL)}\n".encode()
        + TAIL
        + f"#END {ident} {zlib.crc32(TAIL):08x}\n#. {ident} ok\n".encode()
    )


ANSWERS = {
    "info": lambda i: f"#> {i} fw=v0.3.0 proto=1 chip=esp32c3\n#. {i} ok\n".encode(),
    "stats": lambda i: (
        f"#> {i} dropped=42 queue=0/32\n".encode()
        + LOG_W
        + f"#> {i} reply_failed=0\n#. {i} ok\n".encode()
    ),
    "log level APP D": lambda i: f"#. {i} ok\n".encode(),
    "log level X D": lambda i: f"#! {i} full tag table full (16 tags)\n".encode(),
    "log tail": tail_block,
    "reboot": lambda i: f"#. {i} ok\n".encode(),
}


class FakeDevice:
    """A transport whose device answers requests; `silent` commands get nothing back."""

    def __init__(self, *, connect: bool = True, greeting: bytes = b"", restart_on: tuple = ()):
        self._queue: queue.Queue[bytes | Notice | None] = queue.Queue()
        self._connect = connect
        self._greeting = greeting
        self._restart_on = restart_on
        self.written: list[bytes] = []
        self.closed = False

    def chunks(self) -> Iterator[bytes | Notice]:
        if not self._connect:
            yield Notice(
                NoticeKind.WAITING, "waiting for COM6: port busy (is a serial monitor open?)"
            )
            while not self.closed:
                item = self._queue.get()
                if item is None:
                    return
            return
        yield Notice(NoticeKind.CONNECTED, "connected to COM6")
        if self._greeting:
            yield self._greeting
        while True:
            item = self._queue.get()
            if item is None:
                return
            yield item

    def write(self, data: bytes) -> bool:
        if not self._connect or self.closed:
            return False
        self.written.append(data)
        ident, command = data.decode().rstrip("\n").split(" ", 1)
        answer = ANSWERS.get(command)
        if answer is not None:
            self._queue.put(answer(ident))
        if command.split(" ")[0] in self._restart_on:
            self._queue.put(Notice(NoticeKind.DISCONNECTED, "COM6: connection lost"))
        return True

    def close(self) -> None:
        self.closed = True
        self._queue.put(None)


def consoles() -> tuple[Console, Console, io.StringIO, io.StringIO]:
    out, err = io.StringIO(), io.StringIO()
    return (
        Console(file=out, width=200, color_system=None, soft_wrap=True),
        Console(file=err, width=200, color_system=None, soft_wrap=True),
        out,
        err,
    )


def make_shell(
    device: FakeDevice, *, quick: bool = False, **kwargs
) -> tuple[Shell, io.StringIO, io.StringIO, Console, Console]:
    """quick: short timeouts, for the tests that wait for one to expire. The others get long
    ones, so a slow CI machine never turns a reply into a timeout."""
    out_console, err_console, out, err = consoles()
    timeouts = (0.3, 0.5) if quick else (5.0, 5.0)
    exchange = Exchange(RequestIds(start=7), reply_timeout=timeouts[0], restart_timeout=timeouts[1])
    shell = Shell(device, out_console, err_console, exchange=exchange, **kwargs)
    shell.start()
    return shell, out, err, out_console, err_console


@pytest.fixture
def device():
    dev = FakeDevice(greeting=LOG_I)
    yield dev
    dev.close()


def one(
    device: FakeDevice, words: list[str], *, as_json: bool = False, quick: bool = False
) -> tuple[int, str, str]:
    shell, out, err, out_console, err_console = make_shell(device, quick=quick)
    try:
        code = run_one(shell, words, out_console, err_console, as_json=as_json)
    finally:
        shell.close()
    return code, out.getvalue(), err.getvalue()


# ------------------------------------------------------------------ one command


def test_info_prints_aligned_fields(device):
    code, out, err = one(device, ["info"])
    assert code == 0 and err == ""
    assert out.splitlines() == ["fw     v0.3.0", "proto  1", "chip   esp32c3"]
    assert device.written == [b"7 info\n"]


def test_reply_lines_are_collected_across_log_lines(device):
    code, out, _ = one(device, ["stats"])
    assert code == 0
    assert out.splitlines() == ["dropped       42", "queue         0/32", "reply_failed  0"]
    assert "careful" not in out  # one command: no log lines


def test_command_without_data_prints_ok(device):
    assert one(device, ["log", "level", "APP", "D"])[:2] == (0, "ok\n")
    assert device.written == [b"7 log level APP D\n"]


def test_error_reply_goes_to_stderr_with_exit_1(device):
    code, out, err = one(device, ["log", "level", "X", "D"])
    assert code == 1 and out == ""
    assert err == "error: full: tag table full (16 tags)\n"


def test_log_tail_prints_the_block_lines_and_its_check(device):
    code, out, _ = one(device, ["log", "tail"])
    assert code == 0
    assert out.splitlines() == [
        LOG_I.decode().rstrip(),
        LOG_W.decode().rstrip(),
        f"2 lines, {len(TAIL)} bytes, CRC ok",
    ]


def test_no_reply_is_an_error():
    device = FakeDevice(greeting=LOG_I)
    try:
        code, _, err = one(device, ["log", "dump"], quick=True)  # the fake never answers it
    finally:
        device.close()
    assert code == 1 and "no reply in 0.3 s" in err


def test_reboot_waits_for_the_restart():
    device = FakeDevice(greeting=LOG_I, restart_on=("reboot",))
    try:
        code, out, _ = one(device, ["reboot"])
    finally:
        device.close()
    assert code == 0 and out == "ok: the device restarted\n"


def test_crash_succeeds_on_restart_and_fails_without():
    device = FakeDevice(greeting=LOG_I, restart_on=("crash",))
    try:
        assert one(device, ["crash", "panic"])[0] == 0
    finally:
        device.close()
    quiet = FakeDevice(greeting=LOG_I)
    try:
        code, _, err = one(quiet, ["crash", "wdt"], quick=True)
    finally:
        quiet.close()
    assert code == 1 and "no restart seen" in err


def test_port_that_never_opens(monkeypatch):
    device = FakeDevice(connect=False)
    monkeypatch.setattr(shell_module, "CONNECT_TIMEOUT_S", 0.2)
    shell, _, err, out_console, err_console = make_shell(device)
    try:
        code = run_one(shell, ["info"], out_console, err_console, as_json=False)
    finally:
        shell.close()
    assert code == 1 and "port busy" in err.getvalue()


def test_json(device):
    code, out, _ = one(device, ["stats"], as_json=True)
    assert code == 0
    data = json.loads(out)
    assert data["ok"] is True and data["id"] == 7
    assert data["fields"] == {"dropped": 42, "queue": "0/32", "reply_failed": 0}


def test_json_error_exits_1(device):
    code, out, _ = one(device, ["log", "level", "X", "D"], as_json=True)
    assert code == 1 and json.loads(out)["code"] == "full"


# ------------------------------------------------------------------ lines from a pipe


def test_lines_mode_runs_each_command_and_shows_logs(device):
    shell, out, err, out_console, err_console = make_shell(
        device, show_logs=True, line_filter=LogFilter.parse(["*:W"])
    )
    try:
        code = run_lines(
            shell,
            ["info", "", "# a comment", "stats", "bogus", "quit", "info"],
            out_console,
            err_console,
        )
    finally:
        shell.close()
    assert code == 1  # "bogus" failed
    text = out.getvalue()
    assert "fw     v0.3.0" in text and "dropped" in text
    assert "careful" in text and "hello" not in text  # the filter: W shown, I hidden
    assert "unknown command: bogus" in err.getvalue()
    assert [w.split(b" ", 1)[1] for w in device.written] == [b"info\n", b"stats\n"]  # stops at quit


def test_reply_for_nobody_is_shown_as_raw_in_the_interactive_view():
    device = FakeDevice(greeting=LOG_I + b"#. 5 ok\n")
    shell, out, _, out_console, err_console = make_shell(device, show_logs=True)
    try:
        run_lines(shell, ["info"], out_console, err_console)
    finally:
        shell.close()
        device.close()
    assert "#. 5 ok" in out.getvalue()


def test_a_bom_before_the_first_piped_command_is_ignored(device):
    shell, out, err, out_console, err_console = make_shell(device)
    try:
        assert run_lines(shell, ["\ufeffinfo"], out_console, err_console) == 0
    finally:
        shell.close()
    assert "fw" in out.getvalue() and err.getvalue() == ""


def test_cli_help_without_a_device(capsys):
    assert cli.main(["shell", "help"]) == 0
    assert "log level <tag> <L>" in capsys.readouterr().out


def test_help_lists_the_commands(device):
    shell, out, _, out_console, err_console = make_shell(device)
    try:
        assert run_lines(shell, ["help"], out_console, err_console) == 0
    finally:
        shell.close()
    assert "log level <tag> <L>" in out.getvalue()


# ------------------------------------------------------------------ the prompt


def test_prompt_runs_commands_until_eof(device, monkeypatch):
    from prompt_toolkit.application import create_app_session
    from prompt_toolkit.input import create_pipe_input
    from prompt_toolkit.output import DummyOutput

    shell, out, _, out_console, err_console = make_shell(device)
    with create_pipe_input() as pipe, create_app_session(input=pipe, output=DummyOutput()):
        pipe.send_text("info\r")
        pipe.send_text("log level APP D\r")
        pipe.close()
        try:
            code = shell_module.run_prompt(shell, out_console, err_console)
        finally:
            shell.close()
    assert code == 0
    assert "fw     v0.3.0" in out.getvalue() and "ok" in out.getvalue()


# ------------------------------------------------------------------ printing


def test_print_reply_of_a_damaged_block():
    from esp_logcat.console import Reply
    from esp_logcat.events import Block

    reply = Reply(
        "7", "log tail", block=Block("7", "ram", 9, b"abc", None, "no #END after the content")
    )
    reply.failure = "dump block damaged: no #END after the content"
    out_console, err_console, out, err = consoles()
    assert print_reply(reply, out_console, err_console) == 1
    assert "no #END after the content" in out.getvalue()
    assert err.getvalue() == "error: dump block damaged: no #END after the content\n"


# ------------------------------------------------------------------ command line


def test_cli_one_command_through_main(device, monkeypatch, capsys):
    monkeypatch.setattr(cli, "SerialTransport", lambda port, baud: device)
    monkeypatch.setattr(cli, "find_port", lambda ports: "COM6")
    monkeypatch.setattr(cli, "list_ports", lambda: [])
    assert cli.main(["shell", "log", "level", "APP", "D"]) == 0
    assert capsys.readouterr().out == "ok\n"


def test_cli_rejects_json_without_a_command(capsys):
    with pytest.raises(SystemExit) as exc:
        cli.main(["shell", "--json"])
    assert exc.value.code == 2
    assert "--json goes with one command" in capsys.readouterr().err


def test_cli_rejects_words_that_are_neither_command_nor_filter(capsys):
    with pytest.raises(SystemExit):
        cli.main(["shell", "hello"])
    assert "a device command starts with one of" in capsys.readouterr().err


def test_cli_rejects_a_bad_filter(capsys):
    with pytest.raises(SystemExit):
        cli.main(["shell", "APP:X"])
    assert "a device command starts with one of" in capsys.readouterr().err


def test_cli_unknown_option(capsys):
    with pytest.raises(SystemExit):
        cli.main(["shell", "info", "--bogus"])
    assert "unrecognized arguments: --bogus" in capsys.readouterr().err


def test_cli_dash_is_a_word_not_an_option(device, monkeypatch, capsys):
    monkeypatch.setattr(cli, "SerialTransport", lambda port, baud: device)
    monkeypatch.setattr(cli, "find_port", lambda ports: "COM6")
    monkeypatch.setattr(cli, "list_ports", lambda: [])
    cli.main(["shell", "log", "level", "-", "D"])
    assert device.written == [b"7 log level - D\n"] or device.written[0].endswith(
        b" log level - D\n"
    )


# ------------------------------------------------------------------ review of #30


class ScriptedDevice(FakeDevice):
    """Writes answer with a scripted list of chunks instead of ANSWERS."""

    def __init__(self, script, *, write_result: bool = True, **kwargs):
        super().__init__(greeting=LOG_I, **kwargs)
        self._script = list(script)
        self._write_result = write_result

    def write(self, data: bytes) -> bool:
        self.written.append(data)
        ident = data.split(b" ", 1)[0].decode()
        for item in self._script.pop(0) if self._script else []:
            self._queue.put(item(ident) if callable(item) else item)
        return self._write_result


def test_a_reply_ended_by_the_reader_before_the_write_returned_is_not_lost():
    # The port drops right as the command goes out: the reader ends the request first, then
    # write() reports failure. The shell must report the reader's verdict, not crash.
    import time

    device = ScriptedDevice(
        [[Notice(NoticeKind.DISCONNECTED, "COM6: connection lost")]], write_result=False
    )
    shell, *_ = make_shell(device)
    original = device.write

    def slow_write(data):
        result = original(data)
        time.sleep(0.2)  # the reader handles the notice meanwhile
        return result

    device.write = slow_write
    try:
        shell.wait_connected(2)
        reply = shell.request("reboot")
    finally:
        shell.close()
        device.close()
    assert reply.ok and reply.restarted


def test_reader_that_dies_ends_the_waiting_request():
    class Exploding(FakeDevice):
        def chunks(self):
            yield Notice(NoticeKind.CONNECTED, "connected to COM6")
            raise RuntimeError("boom")

    device = Exploding()
    shell, *_ = make_shell(device)
    try:
        shell.wait_connected(2)
        reply = shell.request("info")
    finally:
        shell.close()
    assert not reply.ok and "RuntimeError: boom" in reply.failure
    assert "boom" in shell.reader_error


def test_after_a_reboot_the_next_command_waits_for_the_port():
    connected = Notice(NoticeKind.CONNECTED, "connected to COM6")
    lost = Notice(NoticeKind.DISCONNECTED, "COM6: connection lost")
    device = ScriptedDevice(
        [
            [lambda i: f"#. {i} ok\n".encode(), lost],
            [lambda i: f"#> {i} fw=v0.3.0\n#. {i} ok\n".encode()],
        ]
    )
    shell, out, _, out_console, err_console = make_shell(device, show_logs=False)
    writes = []
    original = device.write

    def write_when_back(data):
        writes.append(shell._connected.is_set())
        if len(writes) == 1:
            result = original(data)
            device._queue.put(connected)  # the port comes back after the reset
            return result
        return original(data)

    device.write = write_when_back
    try:
        code = run_lines(shell, ["reboot", "info"], out_console, err_console)
    finally:
        shell.close()
        device.close()
    assert code == 0 and "fw" in out.getvalue()


def test_ctrl_c_during_a_request_abandons_it(monkeypatch):
    device = FakeDevice(greeting=LOG_I)
    shell, *_ = make_shell(device)

    def interrupted(ident):
        raise KeyboardInterrupt

    monkeypatch.setattr(shell, "_wait", interrupted)
    try:
        shell.wait_connected(2)
        with pytest.raises(KeyboardInterrupt):
            shell.request("log dump")
        assert shell.exchange.pending == []
    finally:
        shell.close()
        device.close()


def test_filtered_lines_count_as_hidden_in_the_summary(device):
    shell, out, _, out_console, err_console = make_shell(
        device, show_logs=True, line_filter=LogFilter.parse(["*:E"])
    )
    try:
        run_lines(shell, ["stats"], out_console, err_console)
    finally:
        shell.close()
    assert shell.stats.hidden >= 1 and "hidden by filter" in shell.stats.summary()


def test_a_newline_inside_a_command_cannot_start_a_second_request(device):
    shell, *_ = make_shell(device)
    try:
        shell.wait_connected(2)
        shell.request("info\n9 reboot")
    finally:
        shell.close()
    assert device.written == [b"7 info 9 reboot\n"]


def test_cli_command_after_a_filter_says_the_command_comes_first(capsys):
    with pytest.raises(SystemExit):
        cli.main(["shell", "*:W", "info"])
    assert "the command comes first" in capsys.readouterr().err
