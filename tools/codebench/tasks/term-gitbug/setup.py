import os, subprocess

# historico git deterministico (datas e autor fixos): o commit que quebra o check.py e o 12o

PRICING = """RATES = [(5, 3.0), (20, 5.0), (50, 9.5)]   # (max weight in kg, price)


def shipping_cost(weight):
    if weight <= 0:
        raise ValueError("weight must be positive")
    for limit, price in RATES:
        if weight <= limit:
            return price
    return 9.5 + 0.2 * (weight - 50)
"""
CHECK = """import sys
from pricing import shipping_cost

cases = [(1, 3.0), (5, 3.0), (6, 5.0), (20, 5.0), (21, 9.5), (50, 9.5), (60, 11.5)]
bad = [(w, e, shipping_cost(w)) for w, e in cases if abs(shipping_cost(w) - e) > 1e-9]
for w, e, g in bad:
    print("FAIL shipping_cost(%s) = %s, expected %s" % (w, g, e))
print("FAIL" if bad else "OK")
sys.exit(1 if bad else 0)
"""


def setup(work):
    env = dict(os.environ, GIT_AUTHOR_NAME="dev", GIT_AUTHOR_EMAIL="dev@example.com", GIT_COMMITTER_NAME="dev",
               GIT_COMMITTER_EMAIL="dev@example.com", GIT_CONFIG_GLOBAL="/dev/null", GIT_CONFIG_SYSTEM="/dev/null")

    def git(*a):
        return subprocess.run(["git", "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null"] + list(a),
                              cwd=work, env=env, capture_output=True, text=True, check=True).stdout.strip()

    count = [0]

    def commit(msg, files):
        for name, text in files.items():
            with open(os.path.join(work, name), "w") as f:
                f.write(text)
        git("add", "-A")
        count[0] += 1
        env["GIT_AUTHOR_DATE"] = env["GIT_COMMITTER_DATE"] = "2026-10-%02dT12:00:00+0000" % count[0]
        git("commit", "-q", "-m", msg)
        return git("rev-parse", "HEAD")

    git("init", "-q", "-b", "main")
    commit("initial: shipping price table", {"pricing.py": PRICING, "check.py": CHECK, "README.md": "# shop\n"})
    commit("docs: describe the weight limits", {"README.md": "# shop\nPrices by weight class.\n"})
    p = PRICING + "\n\ndef tax(amount, rate=0.1):\n    return round(amount * rate, 2)\n"
    commit("add tax helper", {"pricing.py": p})
    commit("notes: todo list", {"notes.txt": "- discounts\n- invoices\n"})
    p += "\n\ndef format_price(x):\n    return \"$%.2f\" % x\n"
    commit("add format_price", {"pricing.py": p})
    commit("changelog: 0.1", {"CHANGELOG.md": "0.1 first version\n"})
    p = p.replace("return round(amount * rate, 2)", "return round(amount * rate, 2)  # rounds to cents")
    commit("fix: shipping edge case in tax rounding comment", {"pricing.py": p})
    commit("notes: more ideas", {"notes.txt": "- discounts\n- invoices\n- coupons\n"})
    p += "\n\ndef discount(amount, pct):\n    return round(amount * (1 - pct / 100), 2)\n"
    commit("add discount helper", {"pricing.py": p})
    commit("changelog: 0.2", {"CHANGELOG.md": "0.1 first version\n0.2 tax, discount\n"})
    commit("readme: usage section", {"README.md": "# shop\nPrices by weight class.\n\nUsage: import pricing\n"})
    # o commit que quebra: mensagem inocente, renomeia variaveis e troca <= por <
    bug = p.replace("    for limit, price in RATES:\n        if weight <= limit:\n            return price",
                    "    for max_kg, cost in RATES:\n        if weight < max_kg:\n            return cost")
    assert bug != p
    bad = commit("refactor: tidy variable names in pricing", {"pricing.py": bug})
    commit("notes: release plan", {"notes.txt": "- discounts\n- invoices\n- coupons\n- release 0.3\n"})
    p = bug + "\n\ndef total(items):\n    return round(sum(items), 2)\n"
    commit("add total helper", {"pricing.py": p})
    commit("changelog: 0.3 draft", {"CHANGELOG.md": "0.1 first version\n0.2 tax, discount\n0.3 total (draft)\n"})
    p = p.replace("# rounds to cents", "# rounds to cents (half even)")
    commit("fix: tax comment wording (rounding)", {"pricing.py": p})
    commit("docs: contributing", {"CONTRIBUTING.md": "Run python3 check.py before pushing.\n"})
    p += "\n\ndef free_shipping(amount):\n    return amount >= 100\n"
    commit("add free_shipping rule", {"pricing.py": p})
    commit("notes: cleanup", {"notes.txt": "- coupons\n- release 0.3\n"})
    head = commit("changelog: 0.3", {"CHANGELOG.md": "0.1 first version\n0.2 tax, discount\n0.3 total, free shipping\n"})
    return dict(vars={"bad": bad, "head": head})
