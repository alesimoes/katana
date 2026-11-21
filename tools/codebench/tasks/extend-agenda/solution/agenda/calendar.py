"""The calendar: adding and removing events, and the queries by day and by range."""
from datetime import date, timedelta

from .errors import AgendaError
from .events import Event, parse_date, parse_time
from .recurrence import REPEATS, occurrences


class Calendar:
    def __init__(self, storage):
        self.storage = storage
        self.next_id, self.events = storage.load()

    def _save(self):
        self.storage.save(self.next_id, self.events)

    def add(self, title, day, time, minutes, repeat=None, until=None):
        title = (title or "").strip()
        if not title:
            raise AgendaError("the title cannot be empty")
        first = parse_date(day)
        start = parse_time(time)
        if minutes < 1 or start + minutes > 24 * 60:
            raise AgendaError("the event must last at least 1 minute and end before midnight")
        if repeat is not None and repeat not in REPEATS:
            raise AgendaError("bad repeat: %r (use %s)" % (repeat, ", ".join(REPEATS)))
        if until is not None:
            if repeat is None:
                raise AgendaError("until needs repeat")
            if parse_date(until) < first:
                raise AgendaError("until is before the event date")
        ev = Event(self.next_id, title, day, time, minutes, repeat, until)
        self.next_id += 1
        self.events.append(ev)
        self._save()
        return ev

    def remove(self, event_id):
        for i, e in enumerate(self.events):
            if e.id == event_id:
                del self.events[i]
                self._save()
                return e
        raise AgendaError("no event with id %d" % event_id)

    def on_day(self, day):
        parse_date(day)
        return sorted((e for e in self.events if occurrences(e, day, day)), key=lambda e: (e.start, e.id))

    def between(self, start, end):
        """(date, event) pairs with start <= date <= end (both inclusive), sorted by date, start time and id"""
        a, b = parse_date(start), parse_date(end)
        if b < a:
            raise AgendaError("the range ends before it starts")
        out = []
        d = a
        while d <= b:
            iso = d.isoformat()
            out.extend((iso, e) for e in self.on_day(iso))
            d += timedelta(days=1)
        return out

    def conflicts(self, day):
        """pairs (a, b), a.id < b.id, of events of that day whose time ranges overlap (touching ends do not overlap)"""
        evs = sorted(self.on_day(day), key=lambda e: e.id)
        out = []
        for i, a in enumerate(evs):
            for b in evs[i + 1:]:
                if a.start < b.end and b.start < a.end:
                    out.append((a, b))
        return out
