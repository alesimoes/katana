import csv

def sum_column(path, name):
    total = 0.0
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            v = (row.get(name) or "").strip()
            total += float(v) if v else 0.0
    return total
