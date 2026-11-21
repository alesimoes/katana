class ReportError(Exception):
    """Base class: the CLI prints the message and exits with 1."""


class LoadError(ReportError):
    pass


class UnknownFormat(ReportError):
    pass
