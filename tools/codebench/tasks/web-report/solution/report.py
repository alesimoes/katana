import sys, urllib.request
base = sys.argv[1].rstrip("/")
rows = urllib.request.urlopen(base + "/data/readings.csv").read().decode().strip().split("\n")[1:]
tot = {}
for r in rows:
    n, _ts, t, q = r.split(";")
    if q == "ok":
        tot.setdefault(n, []).append(int(t))
best = max(tot, key=lambda k: sum(tot[k]) / len(tot[k]))
print("%s %.1f" % (best, sum(tot[best]) / len(tot[best]) / 10))
