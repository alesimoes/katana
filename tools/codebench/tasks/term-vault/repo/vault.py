#!/usr/bin/env python3
"""cofre interativo: faz tres perguntas na entrada padrao e imprime um codigo de acesso"""
import hashlib, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SALT = os.environ.get("VAULT_SALT", "")


def answers():
    d = os.path.join(HERE, "data")
    n_csv = sum(1 for f in os.listdir(d) if f.endswith(".csv"))
    total = sum(int(x) for x in re.findall(r"-?\d+", open(os.path.join(d, "numbers.txt")).read()))
    lines = [l for l in open(os.path.join(d, "poem.txt")).read().split("\n") if l.strip()]
    word = max(lines, key=len).split()[0]
    return [str(n_csv), str(total), word.lower()]


QUESTIONS = ["How many files in data/ have the extension .csv?",
             "What is the sum of all the integers in data/numbers.txt?",
             "What is the first word of the longest line of data/poem.txt (lowercase)?"]


def main():
    right = answers()
    given = []
    for q, a in zip(QUESTIONS, right):
        for attempt in range(3):
            print("Q: " + q, flush=True)
            try:
                s = input("> ").strip()
            except EOFError:
                print("no more input; vault locked")
                return 1
            if s == a:
                given.append(s)
                print("correct", flush=True)
                break
            print("wrong (%d of 3)" % (attempt + 1), flush=True)
        else:
            print("too many wrong answers; vault locked")
            return 1
    print("ACCESS GRANTED code=" + hashlib.sha256((SALT + "|" + "|".join(given)).encode()).hexdigest()[:12])
    return 0


if __name__ == "__main__":
    sys.exit(main())
