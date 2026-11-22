#!/usr/bin/env python3
"""CPU against the Vulkan graph, with an image in the prompt, on a tiny random model (no real weights needed).

  python3 tools/visiontest/gpu.py ./katana
"""
import os, struct, subprocess, sys, tempfile
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
K = os.path.abspath(sys.argv[1]); sys.argv = [sys.argv[0], K, "none"]
src = open(os.path.join(HERE, "oracle.py")).read().split("\nonly = ")[0]
ns = {"__file__": os.path.join(HERE, "oracle.py"), "__name__": "oracle_lib"}
exec(compile(src, "oracle.py", "exec"), ns)
T = tempfile.mkdtemp()
m, pj, ip, f = (os.path.join(T, x) for x in ("tiny.gguf", "mmproj-t.gguf", "i.png", "l.bin"))
subprocess.run([sys.executable, os.path.join(HERE, "..", "tinymodel", "make_tiny_gguf.py"), m, "--arch", "qwen35moe", "--seed", "7", "--vision", "--gpu", "--experts", "64", "--used", "8"], check=True, stdout=subprocess.DEVNULL)
ns["projector"](pj, 32, 72, 2, 4, 128, seed=4)
ns["picture"](150, 110, 2).save(ip)
base = {"KATANA_DUMP_LOGITS": f, "KATANA_SPEC": "", "KATANA_IMAGE_TOKENS": "64", "KATANA_MMPROJ": pj}


def logits(env, image=True, n=1, text="the hello and other in the an ou and then more the hello"):
    if os.path.exists(f): os.remove(f)
    r = subprocess.run([K, "run", m, text, "1", "0"] + ["--image", ip] * (n if image else 0), capture_output=True, text=True, errors="replace", env=dict(os.environ, **dict(base, **env)))
    if not os.path.exists(f): print(r.stderr[-1500:]); return None, r.stderr
    return np.frombuffer(open(f, "rb").read(), np.float32), r.stderr


bad = 0
for n in (0, 1, 3):
    ref, _ = logits({"KATANA_CPU": "1"}, n > 0, n)
    for nm, e in (("attention on the GPU, batch", {"KATANA_QN_CPUATT": "0"}), ("attention on the GPU, KV fp16, batch", {"KATANA_QN_CPUATT": "0", "KATANA_KV16": "1"}), ("attention on the GPU, one by one", {"KATANA_QN_CPUATT": "0", "KATANA_VK_MINB": "99999"}),
                  ("attention on the CPU, batch", {"KATANA_QN_CPUATT": "1"}), ("attention on the CPU, one by one", {"KATANA_QN_CPUATT": "1", "KATANA_VK_MINB": "99999"}),
                  ("small passes", {"KATANA_QN_CPUATT": "0", "KATANA_VK_CAP": "19"})):
        g, err = logits(dict({"KATANA_BACKEND": "vulkan"}, **e), n > 0, n)
        vk = "grafo completo do qwen35moe" in err
        d = np.abs(g - ref).max() / np.abs(ref).max() if g is not None else 9
        ok = vk and d < 1e-4
        bad += not ok
        print("%-4s %d image(s), %s: largest relative difference %.1e, same best token: %s%s" % ("ok" if ok else "FAIL", n, nm, d, g is not None and int(g.argmax()) == int(ref.argmax()), "" if vk else "  (the Vulkan graph did not run)"))
    if n:      # controle: a rotacao de texto no lugar da de imagem tem de dar outro resultado
        z, _ = logits({"KATANA_CPU": "1"}, False, 0)
        print("     (text only differs by %.1e)" % (np.abs(z - ref).max() / np.abs(ref).max()))
print("FAILED %d" % bad if bad else "OK")
sys.exit(1 if bad else 0)
