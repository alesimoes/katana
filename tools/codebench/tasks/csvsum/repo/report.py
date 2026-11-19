def sum_column(path, name):
    with open(path) as f:
        lines = f.read().splitlines()
    header = lines[0].split(",")
    idx = header.index(name)
    total = 0.0
    for line in lines[1:]:
        total += float(line.split(",")[idx])
    return total
