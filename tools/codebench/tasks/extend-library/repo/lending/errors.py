"""Errors raised by the lending service. The CLI turns them into a message and exit code 1."""


class LendingError(Exception):
    pass


class NotFound(LendingError):
    pass


class NotAvailable(LendingError):
    pass


class LimitReached(LendingError):
    pass


class AlreadyReturned(LendingError):
    pass


class BadDate(LendingError):
    pass
