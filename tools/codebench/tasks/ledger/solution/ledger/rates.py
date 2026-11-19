def load_rates(path):
    rates = {"EUR": 1.0}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            code, value = line.split()
            rates[code] = float(value)
    rates["EUR"] = 1.0
    return rates


def convert(amount, currency, rates):
    return round(amount * rates[currency], 2)
