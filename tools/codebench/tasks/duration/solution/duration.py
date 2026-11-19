UNITS = {"d": 86400, "h": 3600, "m": 60, "s": 1}

def parse_duration(text):
    text = text.replace(" ", "")
    if not text:
        raise ValueError("empty")
    total = 0
    num = ""
    for ch in text:
        if ch.isdigit():
            num += ch
        elif ch in UNITS and num:
            total += int(num) * UNITS[ch]
            num = ""
        else:
            raise ValueError("bad duration")
    if num:
        total += int(num)
    return total
