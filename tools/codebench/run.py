#!/usr/bin/env python3
"""codebench: a small local coding benchmark for the katana agent.

Each task (tasks/<name>/) has: instruction.md (what the user asks), repo/ (the starting project, with a bug or a
stub), hidden/ (tests the agent never sees) and solution/ (a reference fix, used only by --verify).

Usage:
  run.py --verify [--tasks a,b]                     check that every task fails before and passes after the reference fix
  run.py --model models/X.gguf --out a.jsonl        run the katana agent on every task, one fresh copy of repo/ each
         [--mode agent|code] [--tasks a,b] [--timeout 900] [--label NAME] [--katana ./katana] [--env KEY=VAL ...]
  run.py compare a.jsonl b.jsonl                    paired comparison (newly solved / lost, exact sign test)

The agent runs in a temporary copy of repo/ as its working folder and receives the instruction as its first message.
Afterwards hidden/ tests are copied in and run with python3 -m unittest. Only the Python standard library is needed.
"""
import argparse, importlib.util, json, math, os, re, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
TASKS = os.path.join(HERE, "tasks")


def task_names(sel=None):
    names = sorted(d for d in os.listdir(TASKS) if os.path.isdir(os.path.join(TASKS, d)))
    return [n for n in names if not sel or n in sel]


def copy_tree(src, dst):
    for root, _dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in files:
            shutil.copy2(os.path.join(root, f), os.path.join(dst, rel, f))


def prepare(work, name):
    """gancho opcional por tarefa: tasks/<name>/setup.py com setup(work) -> dict. Chaves (todas opcionais):
    vars (texto que substitui {chave} no enunciado; tambem vai como CB_<CHAVE> para os testes e a solucao, nunca
    para o agente), agent_env (variaveis de ambiente que o agente tambem recebe), stop (funcao de limpeza, ex.
    parar o servidor local). Sem setup.py devolve um contexto vazio e nada muda."""
    ctx = dict(vars={}, agent_env={}, stop=None)
    path = os.path.join(TASKS, name, "setup.py")
    if os.path.exists(path):
        sys.path.insert(0, HERE)
        spec = importlib.util.spec_from_file_location("cb_setup_" + name.replace("-", "_"), path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        ctx.update(mod.setup(work) or {})
    return ctx


def ctx_env(ctx, agent=False):
    env = dict(os.environ)
    env.update(ctx["agent_env"])
    if not agent:
        env.update({"CB_" + k.upper(): str(v) for k, v in ctx["vars"].items()})
    return env


def finish(ctx):
    if ctx["stop"]:
        ctx["stop"]()


def apply_solution(work, name, ctx):
    """solution/solve.sh (roda na pasta de trabalho, com o ambiente do contexto) ou, sem ele, copia solution/"""
    sol = os.path.join(TASKS, name, "solution")
    script = os.path.join(sol, "solve.sh")
    if os.path.exists(script):
        p = subprocess.run(["bash", script], cwd=work, env=ctx_env(ctx), capture_output=True, text=True, timeout=120)
        if p.returncode != 0:
            raise RuntimeError("solve.sh falhou: " + (p.stdout + p.stderr)[-400:])
    else:
        copy_tree(sol, work)


def hidden_tests(work, name, ctx=None):
    """copy hidden tests into work and run them; returns (passed, output)"""
    copy_tree(os.path.join(TASKS, name, "hidden"), work)
    try:
        p = subprocess.run([sys.executable, "-m", "unittest", "discover", "-p", "test_hidden*.py"], cwd=work,
                           capture_output=True, text=True, timeout=120, env=ctx_env(ctx) if ctx else None)
        return p.returncode == 0, (p.stdout + p.stderr)[-600:]
    except subprocess.TimeoutExpired:
        return False, "hidden tests timed out"


def verify(names):
    bad = 0
    for n in names:
        w = tempfile.mkdtemp(prefix="cb_")
        copy_tree(os.path.join(TASKS, n, "repo"), w)
        ctx = prepare(w, n)
        try:
            leaked = [f for f in os.listdir(w) if f in ("solution", "hidden", "solve.sh", "setup.py", "site")]
            before, _ = hidden_tests(w, n, ctx)
        finally:
            finish(ctx)
        w2 = tempfile.mkdtemp(prefix="cb_")
        copy_tree(os.path.join(TASKS, n, "repo"), w2)
        ctx = prepare(w2, n)
        try:
            apply_solution(w2, n, ctx)
            after, out = hidden_tests(w2, n, ctx)
        finally:
            finish(ctx)
        if leaked:
            out = "solution/hidden material visible to the agent: %s\n" % leaked + out
        ok = (not before) and after and not leaked
        print("%-14s before=%s after=%s %s" % (n, "pass" if before else "fail", "pass" if after else "fail",
                                              "ok" if ok else "BROKEN TASK\n" + out))
        bad += not ok
        shutil.rmtree(w, ignore_errors=True); shutil.rmtree(w2, ignore_errors=True)
    print("%d tasks, %d broken" % (len(names), bad))
    return 1 if bad else 0


def run_task(a, name):
    work = tempfile.mkdtemp(prefix="cb_" + name + "_")
    copy_tree(os.path.join(TASKS, name, "repo"), work)
    ctx = prepare(work, name)
    instr = " ".join(open(os.path.join(TASKS, name, "instruction.md")).read().split())
    for k, v in ctx["vars"].items():
        instr = instr.replace("{" + k + "}", str(v))
    msg = ("You are working in the current folder, a small project (python3 with the standard library and unittest, "
           "and the usual shell tools; there is no pytest). Task: " + instr + " Check your work by running it. "
           "When it is finished, answer with one short sentence.")
    env = ctx_env(ctx, agent=True)
    for kv in a.env or []:
        k, _, v = kv.partition("=")
        env[k] = v
    t0 = time.time()
    timed_out = False
    try:
        p = subprocess.run([a.katana, a.mode, a.model], cwd=work, input=msg + "\n/exit\n", capture_output=True,
                           text=True, timeout=a.timeout, env=env)
        log = p.stdout + "\n" + p.stderr
    except subprocess.TimeoutExpired as e:
        timed_out = True
        log = (e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        log += "\n" + ((e.stderr or b"").decode("utf-8", "replace") if isinstance(e.stderr, bytes) else (e.stderr or ""))
    secs = time.time() - t0
    try:
        passed, out = hidden_tests(work, name, ctx)
    finally:
        finish(ctx)
    tool_calls = len(re.findall(r"\[ferramenta ", log))
    gen = sum(int(x) for x in re.findall(r"resposta: (\d+) tokens", log))
    rec = dict(label=a.label, task=name, passed=passed, seconds=round(secs, 1), tool_calls=tool_calls,
               gen_tokens=gen, timed_out=timed_out)
    if a.keep:
        os.makedirs(a.keep, exist_ok=True)
        open(os.path.join(a.keep, name + ".log"), "w").write(log)
        shutil.copytree(work, os.path.join(a.keep, name), dirs_exist_ok=True)
    shutil.rmtree(work, ignore_errors=True)
    return rec


def sign_test(wins, losses):
    n = wins + losses
    if n == 0:
        return 1.0
    k = min(wins, losses)
    p = sum(math.comb(n, i) for i in range(0, k + 1)) / 2 ** n
    return min(1.0, 2 * p)


def load(path):
    return {r["task"]: r for r in (json.loads(l) for l in open(path) if l.strip())}


def compare(pa, pb):
    A, B = load(pa), load(pb)
    common = sorted(set(A) & set(B))
    wins = [t for t in common if B[t]["passed"] and not A[t]["passed"]]
    loss = [t for t in common if A[t]["passed"] and not B[t]["passed"]]
    sa = sum(A[t]["passed"] for t in common); sb = sum(B[t]["passed"] for t in common)
    print("tasks in common: %d" % len(common))
    print("A (%s): %d/%d solved   B (%s): %d/%d solved" % (pa, sa, len(common), pb, sb, len(common)))
    print("newly solved by B: %d %s" % (len(wins), wins))
    print("lost by B:         %d %s" % (len(loss), loss))
    print("exact sign test p = %.4f (few tasks: treat small differences as noise)" % sign_test(len(wins), len(loss)))
    for k in ("seconds", "tool_calls", "gen_tokens"):
        ma = sum(A[t][k] for t in common) / max(1, len(common)); mb = sum(B[t][k] for t in common) / max(1, len(common))
        print("mean %-11s A %.1f   B %.1f" % (k, ma, mb))


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "compare":
        compare(sys.argv[2], sys.argv[3]); return 0
    ap = argparse.ArgumentParser()
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--model"); ap.add_argument("--out"); ap.add_argument("--tasks")
    ap.add_argument("--timeout", type=int, default=900); ap.add_argument("--label", default="run")
    ap.add_argument("--katana", default=os.path.join(HERE, "..", "..", "katana"))
    ap.add_argument("--mode", default="agent", choices=["agent", "code"], help="katana subcommand to run")
    ap.add_argument("--env", action="append"); ap.add_argument("--keep", help="folder to keep logs and final files")
    a = ap.parse_args()
    a.katana = os.path.abspath(a.katana)
    if a.model:
        a.model = os.path.abspath(a.model)
    names = task_names(a.tasks.split(",") if a.tasks else None)
    if a.verify:
        return verify(names)
    if not a.model or not a.out:
        ap.error("--model and --out are required")
    done = load(a.out) if os.path.exists(a.out) else {}        # an interrupted run continues where it stopped
    for n in names:
        if n in done:
            continue
        rec = run_task(a, n)
        open(a.out, "a").write(json.dumps(rec) + "\n")
        print("%-14s %s  %5.0fs  tools=%d  gen=%d%s" % (n, "PASS" if rec["passed"] else "fail", rec["seconds"],
                                                        rec["tool_calls"], rec["gen_tokens"],
                                                        "  TIMEOUT" if rec["timed_out"] else ""), flush=True)
    rs = load(a.out)
    print("solved %d/%d" % (sum(r["passed"] for r in rs.values()), len(rs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
