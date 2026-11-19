UNITS = {"h": 3600, "m": 60, "s": 1}

def parse_duration(text):
    total = 0
    num = ""
    for ch in text:
        if ch.isdigit():
            num += ch
        else:
            total += int(num) * UNITS[ch]
            num = ""
    return total
