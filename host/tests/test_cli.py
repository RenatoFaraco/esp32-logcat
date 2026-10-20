import pytest

from esp_logcat import __version__
from esp_logcat.cli import SUBCOMMANDS, main


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
