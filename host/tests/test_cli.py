import codecs
import errno
import io
import sys
from collections.abc import Iterator
from pathlib import Path

import pytest

from esp_logcat import __version__, cli
from esp_logcat.cli import SUBCOMMANDS, main, prepare_output, run_stream
from esp_logcat.events import Notice, NoticeKind
from esp_logcat.filters import LogFilter
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


# ---------------------------------------------------------------- filters (#17)

FILTERED = (
    b"ESP-ROM:esp32c3-api1-20210207\r\n"
    b"0000:000001 0000000010 I APP: hello\r\n"
    b"0000:000002 0000000011 W APP: careful\r\n"
    b"0000:000003 0000001000 I HEARTBEAT: tick 1\r\n"
    b"0000:000009 0000002000 I HEARTBEAT: tick 2\r\n"
)


def test_filter_hides_lines_but_not_the_gaps():
    line_filter = LogFilter.parse(["*:S", "HEARTBEAT:I"])
    out, err, code = stream([FILTERED], line_filter=line_filter)
    assert code == 0
    assert out.splitlines() == [
        "ESP-ROM:esp32c3-api1-20210207",  # no tag: always passes
        "0000:000003 0000001000 I HEARTBEAT: tick 1",
        "0000:000009 0000002000 I HEARTBEAT: tick 2",
    ]
    assert err.splitlines()[0] == "-- filter: *:S HEARTBEAT:I --"
    assert "-- 5 lines lost before seq 000009 --" in err  # stats saw every line
    assert err.rstrip().endswith("| 2 hidden by filter")


def test_trivial_filter_prints_no_notice():
    out, err, _ = stream([FILTERED], line_filter=LogFilter.parse(["*:V"]))
    assert "filter" not in err
    assert len(out.splitlines()) == 5


def test_stream_filter_arguments_reach_the_stream(monkeypatch, capsys):
    class FakeTransport:
        def __init__(self, port: str, baudrate: int, *, reset: bool) -> None:
            pass

        def chunks(self) -> Iterator[bytes]:
            yield FILTERED

        def close(self) -> None:
            pass

    monkeypatch.setattr(cli, "SerialTransport", FakeTransport)
    assert main(["stream", "--port", "COM6", "*:W"]) == 0
    captured = capsys.readouterr()
    assert captured.out.splitlines() == [
        "ESP-ROM:esp32c3-api1-20210207",
        "0000:000002 0000000011 W APP: careful",
    ]
    assert "-- filter: *:W --" in captured.err


@pytest.mark.parametrize(("spec", "message"), [("APP:F", "use E"), ("APP:Q", "one of V D I")])
def test_invalid_filter_is_a_usage_error(spec, message, capsys):
    with pytest.raises(SystemExit) as exc:
        main(["stream", spec])
    assert exc.value.code == 2
    assert message in capsys.readouterr().err


def test_filters_and_options_in_any_order(monkeypatch, capsys):
    seen = {}

    class FakeTransport:
        def __init__(self, port: str, baudrate: int, *, reset: bool) -> None:
            pass

        def chunks(self) -> Iterator[bytes]:
            yield FILTERED

        def close(self) -> None:
            pass

    real_run_stream = cli.run_stream

    def spy(*args, **kwargs):
        seen["filter"] = kwargs["line_filter"].describe()
        return real_run_stream(*args, **kwargs)

    monkeypatch.setattr(cli, "SerialTransport", FakeTransport)
    monkeypatch.setattr(cli, "run_stream", spy)
    assert main(["stream", "*:S", "--port", "COM6", "APP:D", "--time"]) == 0
    assert seen["filter"] == "*:S APP:D"


def test_unknown_option_is_still_an_error(capsys):
    with pytest.raises(SystemExit) as exc:
        main(["stream", "*:W", "--bogus"])
    assert exc.value.code == 2
    assert "unrecognized arguments: --bogus" in capsys.readouterr().err


def test_filter_error_shows_the_stream_usage(capsys):
    with pytest.raises(SystemExit):
        main(["stream", "APP:Q"])
    assert "usage: esp-logcat stream" in capsys.readouterr().err


def test_backend_modules_never_import_rich_or_textual():
    # ADR-008 layers: only render (rich) and tui/ (textual) may import UI libraries.
    import subprocess
    import sys

    code = (
        "import sys\n"
        "import esp_logcat.events, esp_logcat.filters, esp_logcat.parser\n"
        "import esp_logcat.pipeline, esp_logcat.stats, esp_logcat.transport\n"
        "bad = [m for m in ('rich', 'textual') if m in sys.modules]\n"
        "assert not bad, bad\n"
    )
    subprocess.run([sys.executable, "-c", code], check=True)


# ---------------------------------------------------------------- replay (#19)

TWO_BOOTS = Path(__file__).parent / "fixtures" / "two_boots_esp32c3.log"


def replay(argv: list[str], capsys) -> tuple[str, str, int]:
    code = main(["replay", *argv])
    captured = capsys.readouterr()
    return captured.out, captured.err, code


def test_replay_prints_the_file_like_stream_does(capsys):
    out, err, code = replay([str(TWO_BOOTS)], capsys)
    assert code == 0
    assert out == TWO_BOOTS.read_bytes().replace(b"\r\n", b"\n").decode()
    assert err.splitlines()[0] == f"-- replaying {TWO_BOOTS} --"
    assert "-- device restarted (boot 0000 seq 000110 -> boot 0000 seq 000000) --" in err
    summary = err.splitlines()[-1]
    assert "189 lines, 77 log, 92 esp-idf, 20 raw, 0 invalid" in summary
    assert "lost 144" in summary and "restarts 1" in summary


def test_replay_filters(capsys):
    out, err, _ = replay([str(TWO_BOOTS), "*:W"], capsys)
    logcat_lines = [line for line in out.splitlines() if line[:4].isdigit()]
    assert {line.split()[2] for line in logcat_lines} == {"W", "E"}
    assert "0000:000106 0000000182 W LOGCAT: dropped 72 lines: log queue full" in logcat_lines
    assert "-- filter: *:W --" in err
    assert err.rstrip().endswith("hidden by filter")


def test_replay_colors_on_a_terminal(capsys, monkeypatch):
    def terminal(file):
        return cli._Console(file=file, force_terminal=True, color_system="standard", soft_wrap=True)

    monkeypatch.setattr(cli, "_console", terminal)
    out, _, _ = replay([str(TWO_BOOTS), "*:S", "APP:W"], capsys)
    error = next(line for line in out.splitlines() if "an error, letter E" in line)
    warning = next(line for line in out.splitlines() if "a warning, letter W" in line)
    assert "\x1b[1;31m" in error  # bold red: E
    assert "\x1b[1;33m" in warning  # bold yellow: W


def test_replay_missing_file_is_a_usage_error(capsys, tmp_path):
    with pytest.raises(SystemExit) as exc:
        main(["replay", str(tmp_path / "nope.log")])
    assert exc.value.code == 2
    err = capsys.readouterr().err
    assert "usage: esp-logcat replay" in err and "cannot read" in err


def test_replay_strict_by_default_lenient_on_request(capsys, tmp_path):
    path = tmp_path / "edited.log"
    path.write_bytes(b"0000:000001 0000000010 I APP:\n")  # empty message lost its space
    out, err, _ = replay([str(path)], capsys)
    assert out.startswith("!! bad_header:")
    out, err, _ = replay([str(path), "--lenient"], capsys)
    assert out == "0000:000001 0000000010 I APP: \n"
    assert "0 invalid" in err


def test_replay_reads_stdin(capsys, monkeypatch):
    stdin = io.TextIOWrapper(io.BytesIO(b"0000:000001 0000000010 E APP: piped\n"))
    monkeypatch.setattr(sys, "stdin", stdin)
    out, err, code = replay(["-"], capsys)
    assert code == 0
    assert out == "0000:000001 0000000010 E APP: piped\n"
    assert "-- replaying stdin --" in err


def test_replay_reads_a_powershell_utf16_file(capsys, tmp_path):
    path = tmp_path / "saved-by-powershell.log"
    text = TWO_BOOTS.read_bytes().decode("ascii")
    path.write_bytes(codecs.BOM_UTF16_LE + text.encode("utf-16-le"))
    out, _, _ = replay([str(path)], capsys)
    assert out == text.replace("\r\n", "\n")


def test_replay_filters_after_an_option(capsys):
    out, err, code = replay([str(TWO_BOOTS), "--lenient", "*:S", "LOGCAT:W"], capsys)
    assert code == 0
    assert "-- filter: *:S LOGCAT:W --" in err
    assert out.count("W LOGCAT: dropped 72 lines") == 2


def test_replay_of_a_file_saved_with_time(capsys, tmp_path):
    path = tmp_path / "timed.log"
    path.write_bytes(
        b"10:00:00.001 0000:000001 0000000010 E APP: boom\n"
        b"10:00:00.002 0000:000002 0000000011 I APP: fine\n"
    )
    out, err, _ = replay([str(path), "*:E"], capsys)
    assert out == "0000:000001 0000000010 E APP: boom\n"  # parsed, filtered, time dropped
    assert "2 lines, 2 log" in err


def test_replay_lenient_accepts_a_missing_final_newline(capsys, tmp_path):
    path = tmp_path / "edited.log"
    path.write_bytes(b"0000:000001 0000000010 I APP: last line")
    out, _, _ = replay([str(path)], capsys)
    assert out.startswith("!! torn:")  # strict: an unterminated line is torn
    out, _, _ = replay([str(path), "--lenient"], capsys)
    assert out == "0000:000001 0000000010 I APP: last line\n"


def test_replay_read_error_exits_non_zero(capsys, monkeypatch):
    class Failing(io.RawIOBase):
        def readable(self) -> bool:
            return True

        def read(self, n: int = -1) -> bytes:
            raise OSError(errno.EINVAL, "Invalid argument")

    monkeypatch.setattr(
        cli.FileTransport, "open", classmethod(lambda cls, path: cls(Failing(), path))
    )
    out, err, code = replay(["lost-drive.log"], capsys)
    assert code == 1
    assert "-- lost-drive.log: read error (Invalid argument) --" in err


def test_replay_directory_error(capsys, tmp_path):
    with pytest.raises(SystemExit) as exc:
        main(["replay", str(tmp_path)])
    assert exc.value.code == 2
    assert "is a directory" in capsys.readouterr().err


def test_replay_filter_before_file_gets_a_hint(capsys):
    with pytest.raises(SystemExit):
        main(["replay", "*:W", str(TWO_BOOTS)])
    err = capsys.readouterr().err
    assert "the file comes first" in err
    assert "*:W looks like a filter" in err


def test_replay_missing_file_shows_the_path_as_typed(capsys, tmp_path):
    missing = tmp_path / "sub" / "x.log"
    with pytest.raises(SystemExit):
        main(["replay", str(missing)])
    assert f"cannot read {missing}:" in capsys.readouterr().err  # not repr()-escaped
