x = 1; y = 2
def one(a): return a + 1
def two(a, *rest, key=None, **kw):
    return (a, rest, key, kw)
async def fetch(u):
    return u
lam = lambda q: q + 1
class Empty: pass
s = """multi
def not_a_def():
    pass
"""
def three(a):
    return one(a) + y
