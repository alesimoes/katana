def flatten(x):
    out = []
    for item in x:
        if hasattr(item, "__iter__"):
            out.extend(flatten(item))
        else:
            out.append(item)
    return out
