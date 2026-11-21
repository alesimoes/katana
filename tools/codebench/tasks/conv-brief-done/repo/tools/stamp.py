import hashlib, os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def digest(root):
    h = hashlib.sha256()
    paths = []
    for d, dirs, files in os.walk(os.path.join(root, "src")):
        dirs[:] = [x for x in dirs if x != "__pycache__"]
        paths += [os.path.join(d, f) for f in files if f.endswith(".py")]
    for p in sorted(paths):
        h.update(os.path.relpath(p, root).encode() + b"\0" + open(p, "rb").read() + b"\0")
    return h.hexdigest()


if __name__ == "__main__":
    os.makedirs(os.path.join(ROOT, ".katana", "state"), exist_ok=True)
    with open(os.path.join(ROOT, ".katana", "state", "verified"), "w") as f:
        f.write(digest(ROOT) + "\n")
