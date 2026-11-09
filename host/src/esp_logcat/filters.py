"""logcat-style filters: which lines to show. Pure, no I/O, no rich (ADR-008).

Specs follow `adb logcat` (PRD H2):

    TAG:L      show TAG at level L or more severe; L is V D I W E, or S/N to silence it
    TAG        same as TAG:V
    *:L        the level for every tag not listed (default: V, i.e. show everything)

The most specific spec wins (an exact tag beats `*`); a tag given twice keeps the last one.
`S` is adb's "silent", `N` the firmware's LogLevel::None; both mean "show nothing".
Specs are whitespace-separated, so one argument may hold several (`"*:S HEARTBEAT:I"`, the
text of the `-- filter: ... --` notice, or a TUI input box); tags cannot contain spaces or ':'.

Only lines with a tag and a level are filtered (esp32-logcat and ESP-IDF lines). Raw text,
command replies, dump markers and invalid lines always pass: they carry no tag, and an invalid
line is a diagnostic. Filtering is display only: the pipeline and its statistics still see every
line, so hiding a tag never looks like lost lines.
"""

from collections.abc import Iterable, Mapping
from dataclasses import dataclass, field
from types import MappingProxyType

from esp_logcat.parser import IdfLine, Line, LogLine

# Lower = more severe, like the firmware's LogLevel (ESP-IDF numbering).
SEVERITY: Mapping[str, int] = MappingProxyType({"E": 1, "W": 2, "I": 3, "D": 4, "V": 5})
SILENT = 0
VERBOSE = SEVERITY["V"]
WILDCARD = "*"
_THRESHOLDS = {**SEVERITY, "S": SILENT, "N": SILENT}
_LETTERS = "V D I W E S N"


class FilterSpecError(ValueError):
    """A filter spec that cannot be parsed; the message is meant for the user."""


@dataclass(frozen=True, slots=True)
class LogFilter:
    """Immutable and hashable: a frontend swaps the whole object to change the filter (the TUI
    replaces it from its UI thread while a worker reads it, with no lock)."""

    default: int = VERBOSE
    tags: Mapping[str, int] = field(default_factory=dict)

    def __post_init__(self) -> None:
        tags = dict(self.tags)  # copy: never alias the caller's dict
        for threshold in (self.default, *tags.values()):
            if threshold not in _THRESHOLDS.values():
                raise ValueError(f"threshold out of range: {threshold!r}")
        object.__setattr__(self, "tags", MappingProxyType(tags))

    def __hash__(self) -> int:
        return hash((self.default, frozenset(self.tags.items())))

    @classmethod
    def parse(cls, specs: str | Iterable[str]) -> "LogFilter":
        """Builds a filter from specs: an iterable of arguments, or one string; each argument
        may hold several whitespace-separated specs."""
        if isinstance(specs, str):
            specs = [specs]
        default = VERBOSE
        tags: dict[str, int] = {}
        for argument in specs:
            for spec in argument.split():
                tag, threshold = _parse_spec(spec)
                if tag == WILDCARD:
                    default = threshold
                else:
                    tags.pop(tag, None)  # re-insert: describe() keeps the order of the last spec
                    tags[tag] = threshold
            if not argument.split():
                raise FilterSpecError(f"invalid filter {argument!r}: empty")
        return cls(default=default, tags=tags)

    @property
    def is_trivial(self) -> bool:
        """True when every line passes (no spec, or only `*:V`-like ones)."""
        return self.default == VERBOSE and all(t == VERBOSE for t in self.tags.values())

    def accepts(self, line: Line) -> bool:
        if not isinstance(line, LogLine | IdfLine):
            return True
        severity = SEVERITY.get(line.level)
        if severity is None:  # never produced by the parser; show rather than hide
            return True
        return severity <= self.tags.get(line.tag, self.default)

    def describe(self) -> str:
        """The filter as specs; `LogFilter.parse(f.describe()) == f`."""
        parts = [f"{WILDCARD}:{_letter(self.default)}"] if self.default != VERBOSE else []
        parts += [f"{tag}:{_letter(threshold)}" for tag, threshold in self.tags.items()]
        return " ".join(parts) or f"{WILDCARD}:V"


def _parse_spec(spec: str) -> tuple[str, int]:
    tag, colon, letter = spec.rpartition(":")
    if not colon:
        tag, letter = spec, "V"  # adb: a bare tag means TAG:V
    if not tag:
        raise FilterSpecError(f"invalid filter {spec!r}: missing tag (use * for every tag)")
    if ":" in tag:
        raise FilterSpecError(f"invalid filter {spec!r}: a tag cannot contain ':'")
    if not letter.isascii():  # "ſ".upper() == "S": only the documented letters
        raise FilterSpecError(f"invalid filter {spec!r}: level must be one of {_LETTERS}")
    letter = letter.upper()
    if letter == "F":
        raise FilterSpecError(
            f"invalid filter {spec!r}: ESP-IDF has no F (fatal) level; use E for errors"
        )
    if letter not in _THRESHOLDS:
        raise FilterSpecError(f"invalid filter {spec!r}: level must be one of {_LETTERS}")
    return tag, _THRESHOLDS[letter]


def _letter(threshold: int) -> str:
    if threshold == SILENT:
        return "S"
    return next(letter for letter, value in SEVERITY.items() if value == threshold)
