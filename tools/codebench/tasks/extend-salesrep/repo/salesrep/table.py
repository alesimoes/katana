"""A table of text cells, the common shape that every output format renders."""
from dataclasses import dataclass


@dataclass
class Table:
    headers: list
    rows: list      # list of lists of str
    align: list     # one of "l" or "r" per column

    def __post_init__(self):
        n = len(self.headers)
        if len(self.align) != n or any(len(r) != n for r in self.rows):
            raise ValueError("every row and the align list need one item per header")
