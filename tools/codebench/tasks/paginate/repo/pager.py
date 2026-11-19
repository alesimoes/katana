def paginate(items, page, size):
    """Return the items of the 1-based page `page`."""
    start = page * size
    return items[start:start + size]
