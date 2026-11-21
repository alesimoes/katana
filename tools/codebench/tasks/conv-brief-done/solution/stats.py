def mean(values):
    if not values:
        raise ValueError("empty")
    return sum(values) / len(values)


def median(values):
    if not values:
        raise ValueError("empty")
    s = sorted(values)
    n = len(s)
    if n % 2:
        return s[n // 2]
    return (s[n // 2 - 1] + s[n // 2]) / 2
