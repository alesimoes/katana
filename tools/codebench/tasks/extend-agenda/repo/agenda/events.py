"""The Event data class and the parsing helpers. Dates are YYYY-MM-DD strings, times are HH:MM strings."""
import re
from dataclasses import dataclass, asdict
from datetime import date

from .errors import AgendaError

_TIME = re.compile(r"(\d\d):(\d\d)")


def parse_date(text):
    try:
        return date.fromisoformat(text)
    except (TypeError, ValueError):
        raise AgendaError("bad date: %r (use YYYY-MM-DD)" % (text,))


def parse_time(text):
    """HH:MM -> minutes since midnight"""
    m = _TIME.fullmatch(text or "")
    if not m or int(m.group(1)) > 23 or int(m.group(2)) > 59:
        raise AgendaError("bad time: %r (use HH:MM)" % (text,))
    return int(m.group(1)) * 60 + int(m.group(2))


def fmt_time(minutes):
    return "%02d:%02d" % divmod(minutes, 60)


@dataclass
class Event:
    id: int
    title: str
    date: str
    time: str
    minutes: int

    @property
    def start(self):
        return parse_time(self.time)

    @property
    def end(self):
        return self.start + self.minutes

    def to_dict(self):
        return asdict(self)

    @classmethod
    def from_dict(cls, d):
        return cls(d["id"], d["title"], d["date"], d["time"], d["minutes"])
