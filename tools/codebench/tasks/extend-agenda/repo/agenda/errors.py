class AgendaError(Exception):
    """Anything the user can fix: bad date, bad time, unknown id. The CLI prints it and exits with 1."""
