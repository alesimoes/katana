"""Text formats used by the CLI."""


def loan_line(loan, store):
    b = store.book(loan.book_id)
    m = store.member(loan.member_id)
    state = "open" if loan.is_open else "returned " + loan.returned_on
    return "loan %d: %s -> %s, borrowed %s, %s" % (loan.id, b.title if b else "?", m.name if m else "?", loan.borrowed_on, state)


def book_line(book, available):
    return "book %d: %s by %s (%d of %d available)" % (book.id, book.title, book.author, available, book.copies)


def member_line(member, open_count):
    return "member %d: %s (%d open)" % (member.id, member.name, open_count)
