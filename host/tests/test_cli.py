import io
from collections.abc import Iterator

import pytest

from esp_logcat import __version__, cli
from esp_logcat.cli import SUBCOMMANDS, main, prepare_output, run_stream
from esp_logcat.events import Notice, NoticeKind
from esp_logcat.transport import PortInfo

LONG = b"0000:000002 0000000020 W PRESSURE: " + b"v" * 124 + b"...\r\n"
DEVICE = (
    b"I (253) APP: before the hook\r\n"
    b"0000:000001 0000000010 I APP: hello [bold]x[/]\r\n"
    + LONG
    + b"0000:000009 0000000030 E APP: late\r\n"
)


def device_text() -> str:
    return DEVICE.replace(b"\r\n", b"\n").decode()


def stream(chunks: list[bytes | Notice], **kwargs: object) -> tuple[str, str, int]:
    out, err = io.StringIO(), io.StringIO()
    code = run_stream(chunks, out, err, **kwargs)  # type: ignore[arg-type]
    return out.getvalue(), err.getvalue(), code


def test_stdout_gets_exactly_the_device_lines():
    out, _, code = stream([Notice(NoticeKind.CONNECTED, "connected to COM6"), DEVICE])
    assert code == 0
    assert out == device_text()  # no wrapping at 80 columns, no markup, no ANSI


def test_notices_and_summary_go_to_stderr():
    _, err, _ = stream([Notice(NoticeKind.CONNECTED, "connected to COM6"), DEVICE])
    assert "-- connected to COM6 --" in err
    assert "-- 6 lines lost before seq 000009 --" in err
    assert "4 lines, 3 log, 1 esp-idf" in err.splitlines()[-1]


def test_pc_time_column():
    out, _, _ = stream([b"0000:000001 0000000010 I APP: hi\n"], show_time=True)
    assert out[2] == ":" and out[5] == ":" and out[8] == "."
    assert out.endswith(" 0000:000001 0000000010 I APP: hi\n")


def test_ctrl_c_prints_the_summary_and_exits_cleanly():
    def interrupted() -> Iterator[bytes]:
        yield b"0000:000001 0000000010 I APP: hi\n"
        raise KeyboardInterrupt

    out, err, code = stream(interrupted())  # type: ignore[arg-type]
    assert code == 0
    assert out == "0000:000001 0000000010 I APP: hi\n"
    assert "1 lines" in err


def test_cp1252_pipe_survives_invalid_utf8_from_the_device():
    raw = io.BytesIO()
    out = io.TextIOWrapper(raw, encoding="cp1252", newline="\n")
    prepare_output(out)
    run_stream([b"bad \xff byte \xc3\xa7\n"], out, io.StringIO())
    out.flush()
    assert raw.getvalue() == b"bad ? byte \xe7\n"  # U+FFFD replaced, the c-cedilla kept


def test_stream_command_uses_the_transport(monkeypatch, capsys):
    created = {}

    class FakeTransport:
        def __init__(self, port: str, baudrate: int, *, reset: bool) -> None:
            created.update(port=port, baudrate=baudrate, reset=reset)
            self.closed = False

        def chunks(self) -> Iterator[bytes]:
            yield b"0000:000001 0000000010 I APP: hi\r\n"

        def close(self) -> None:
            created["closed"] = True

    monkeypatch.setattr(cli, "SerialTransport", FakeTransport)
    monkeypatch.setattr(cli, "list_ports", lambda: [PortInfo("COM6", 0x303A, 0x1001, "AA")])
    assert main(["stream", "--reset"]) == 0
    assert created == {"port": "COM6", "baudrate": 115200, "reset": True, "closed": True}
    assert capsys.readouterr().out == "0000:000001 0000000010 I APP: hi\n"


def test_stream_without_a_clear_port_fails_with_a_hint(monkeypatch, capsys):
    monkeypatch.setattr(cli, "list_ports", lambda: [])
    assert main(["stream"]) == 2
    assert "--port" in capsys.readouterr().err


def test_ports_lists_the_native_port(monkeypatch, capsys):
    monkeypatch.setattr(
        cli,
        "list_ports",
        lambda: [
            PortInfo("COM5", 0x1A86, 0x7522, None, "1-3.1", "USB-SERIAL CH340K (COM5)"),
            PortInfo("COM6", 0x303A, 0x1001, "80:53:E0:0A:5D:AC", None, "Dispositivo Serial USB"),
        ],
    )
    assert main(["ports"]) == 0
    lines = capsys.readouterr().out.splitlines()
    assert lines[0] == "COM5  1A86:7522  USB-SERIAL CH340K (COM5)"
    assert lines[1] == (
        "COM6  303A:1001  SER=80:53:E0:0A:5D:AC  native USB-Serial-JTAG  Dispositivo Serial USB"
    )


def test_version_flag_prints_package_version(capsys):
    with pytest.raises(SystemExit) as exc:
        main(["--version"])
    assert exc.value.code == 0
    assert __version__ in capsys.readouterr().out


def test_no_command_prints_help_and_returns_usage_error(capsys):
    assert main([]) == 2
    assert "usage: esp-logcat" in capsys.readouterr().out


@pytest.mark.parametrize("command", sorted(SUBCOMMANDS))
def test_placeholder_subcommand_reports_not_implemented(command, capsys):
    assert main([command]) == 1
    assert "not implemented" in capsys.readouterr().err


def test_unknown_command_is_rejected():
    with pytest.raises(SystemExit) as exc:
        main(["bogus"])
    assert exc.value.code == 2


class ClosedPipe(io.StringIO):
    """stdout whose reader went away ("esp-logcat stream | head")."""

    def __init__(self, error: OSError) -> None:
        super().__init__()
        self.error = error

    def write(self, text: str) -> int:
        raise self.error


@pytest.mark.parametrize(
    "error", [BrokenPipeError(32, "Broken pipe"), OSError(22, "Invalid argument")]
)
def test_closed_pipe_ends_quietly_and_closes_the_source(error):
    closed = []

    def source() -> Iterator[bytes]:
        try:
            while True:
                yield b"0000:000001 0000000010 I APP: hi\n"
        finally:
            closed.append(True)

    err = io.StringIO()
    assert run_stream(source(), ClosedPipe(error), err) == 0
    assert closed == [True]
    assert "lines" in err.getvalue()


def test_other_output_errors_are_not_swallowed():
    with pytest.raises(OSError):
        run_stream([b"x\n"], ClosedPipe(OSError(28, "No space left on device")), io.StringIO())


@pytest.mark.parametrize("baud", ["0", "-1", "fast"])
def test_baud_must_be_a_positive_integer(baud, capsys):
    with pytest.raises(SystemExit) as exc:
        main(["stream", "--baud", baud])
    assert exc.value.code == 2
    assert "positive integer" in capsys.readouterr().err


def test_ports_escapes_the_device_description(monkeypatch, capsys):
    evil = PortInfo("ttyACM0", 0x303A, 0x1001, "AA", None, "board\x1b]0;title\x07")
    monkeypatch.setattr(cli, "list_ports", lambda: [evil])
    assert main(["ports"]) == 0
    out = capsys.readouterr().out
    assert "\x1b" not in out
    assert "board\\x1b]0;title\\x07" in out
