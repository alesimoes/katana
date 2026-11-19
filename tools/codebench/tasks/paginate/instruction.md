The function paginate(items, page, size) in pager.py is wrong: page 1 must return the first `size` items, page 2 the next ones, and so on (pages start at 1). Fix it.
