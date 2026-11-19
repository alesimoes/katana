PUNCT = ".,!?;:\"'"
def count_words(text):
    counts = {}
    for w in text.split():
        w = w.strip(PUNCT).lower()
        if w:
            counts[w] = counts.get(w, 0) + 1
    return counts
