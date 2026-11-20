from bench_lib import wikidata_holder


def setup(work):
    return dict(vars={"truth": "|".join(wikidata_holder("Q237", "P35"))})
