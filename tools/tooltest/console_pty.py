#!/usr/bin/env python3
"""Drives the katana console inside a pseudo-terminal (rich output, line editor, history) with the tiny model and the
scripted stand-in teacher. Used by tools/tooltest/launch.sh.

  console_pty.py KATANA MODEL WORKDIR HOME MOCK_TEACHER_PROG.PY

Prints one line per check: "ok   <what>" or "FAIL <what>", then "pty: N checks, M failures"."""
import os, pty, re, select, sys, time

katana, model, work, home, mock = sys.argv[1:6]
checks = fails = 0


def check(what, cond, detail=""):
    global checks, fails
    checks += 1
    if cond:
        print("ok   " + what)
    else:
        fails += 1
        print("FAIL " + what)
        if detail:
            print("---- saida:\n" + detail[-1800:] + "\n----")


env = dict(os.environ, HOME=home, TERM="xterm", KATANA_CPU="1", KATANA_TEACHER="1", MOCK_TOOLS="2",
           KATANA_TEACHER_CMD="python3 " + mock, KATANA_LOG=os.path.join(home, "agente.log"))
env.pop("NO_COLOR", None); env.pop("KATANA_UI", None)
os.makedirs(work, exist_ok=True)

pid, fd = pty.fork()
if pid == 0:
    os.chdir(work)
    os.environ.clear(); os.environ.update(env)
    os.execv(katana, [katana, "--model", model, "grave e confira"])
buf = b""


def pump(until, timeout=60):
    """read until the regex (on the text read since the last mark) matches"""
    global buf
    end = time.time() + timeout
    while time.time() < end:
        if re.search(until, re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", buf[mark[0]:].decode("utf-8", "replace")), re.S):
            return True
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try:
                d = os.read(fd, 65536)
            except OSError:
                return False
            if not d:
                return False
            buf += d
    return False


mark = [0]
ok = pump(r"you> .*you> ", 90)        # a primeira mensagem roda e o prompt volta
text = buf.decode("utf-8", "replace")
plain = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", text)
check("o primeiro pedido roda e o prompt volta", ok, plain)
for w in ("katana", "agent console", "folder", "model", "context", "mcp", "skills", "project", "/help lists the commands"):
    check("cabecalho: " + w, w in plain, plain)
check("cabecalho: a pasta de trabalho", work in plain.replace("\r", ""), plain)
check("a primeira mensagem aparece depois do prompt", "grave e confira" in plain, plain)
check("ferramenta: Write com marca de sucesso", re.search(r"✓ Write", plain) is not None, plain)
check("ferramenta: Edit, Bash e Grep", all(re.search("✓ " + n, plain) for n in ("Edit", "Bash", "Grep")), plain)
check("ferramenta: o tempo de cada uma", re.search(r"Write.*\d+(\.\d+)?s", plain) is not None, plain)
check("ferramenta: sem as linhas antigas entre colchetes", "[ferramenta" not in plain, plain)
check("a chamada de ferramenta em XML nao aparece", "<tool_call>" not in plain and "<function=" not in plain, plain)
check("resposta final aparece", "Pronto: ferramentas novas conferidas." in plain, plain)
check("linha de estado do pedido", re.search(r"in · .* out · .* tok/s · context ", plain) is not None, plain)
check("o tom das cores (ANSI) esta presente no terminal", "\x1b[" in text)

mark[0] = len(buf)
os.write(fd, b"/status\r")
check("/status responde", pump(r"project.*you> ", 30) and "mcp" in buf[mark[0]:].decode("utf-8", "replace"), buf[mark[0]:].decode("utf-8", "replace"))
st1 = buf[mark[0]:].decode("utf-8", "replace")
check("/status mostra modelo, contexto, sessao, mcp, skills e projeto", all(w in st1 for w in ("model", "context", "session", "mcp", "skills", "project")), st1)
mark[0] = len(buf)
os.write(fd, b"\x1b[A")            # seta para cima: recupera o /status do historico
time.sleep(0.5)
os.write(fd, b"\r")
check("historico: seta para cima repete o comando", pump(r"project.*you> ", 30) and "session" in buf[mark[0]:].decode("utf-8", "replace"), buf[mark[0]:].decode("utf-8", "replace"))
mark[0] = len(buf)
os.write(fd, b"/help\r")
pump(r"stays one message.*you> ", 30)
hp = buf[mark[0]:].decode("utf-8", "replace")
for c in ("/new", "/clear", "/compact", "/plan", "/go", "/undo", "/cd", "/session", "/resume", "/teacher", "/model", "/status", "/help", "/exit"):
    check("/help lista " + c, c in hp, hp)
mark[0] = len(buf)
# edicao: escreve, volta com a seta esquerda, apaga com backspace e confirma
os.write(fd, b"/statusX")
time.sleep(0.3)
os.write(fd, b"\x7f\r")
check("edicao: backspace antes de confirmar", pump(r"project.*you> ", 30) and "unknown" not in buf[mark[0]:].decode("utf-8", "replace"), buf[mark[0]:].decode("utf-8", "replace"))
mark[0] = len(buf)
# colagem de varias linhas (marcas de colagem entre colchetes): uma mensagem so; aqui um comando para nao chamar o modelo
os.write(fd, b"\x1b[200~/model\nlinha dois\x1b[201~")
time.sleep(0.4)
os.write(fd, b"\r")
pump(r"(unknown command|current).*you> ", 30)
pt = buf[mark[0]:].decode("utf-8", "replace")
check("colagem: o texto colado vira uma linha so (uma resposta de comando, nao duas)", pt.count("unknown command") == 1 and "linha dois" not in pt.split("unknown command")[1], pt)
check("colagem: mostra o simbolo de quebra", "↵" in pt, pt)
mark[0] = len(buf)
os.write(fd, b"/model\r")
pump(r"switches.*you>", 30)
mt = buf[mark[0]:].decode("utf-8", "replace")
check("/model lista os modelos instalados", "agent.gguf" in mt and "second.gguf" in mt, mt)
mark[0] = len(buf)
os.write(fd, b"/model second\r")
swok = pump(r"switching to .*second\.gguf.*model +\S*second\.gguf.*sessao retomada.*you>", 180)
sw = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", buf[mark[0]:].decode("utf-8", "replace"))
check("/model troca o modelo: o motor recarrega e o cabecalho mostra o novo", swok, sw)
check("/model: a conversa continua (sessao retomada)", "sessao retomada" in sw, sw)
mark[0] = len(buf)
os.write(fd, b"/exit\r")
time.sleep(1.0)
try:
    os.waitpid(pid, 0)
except OSError:
    pass
hist = ""
for root, _, files in os.walk(os.path.join(home, ".katana", "sessions")):
    if "history" in files:
        hist = open(os.path.join(root, "history")).read()
check("historico da pasta gravado", "/status" in hist, hist)
check("historico guarda a colagem em uma linha", any(l.startswith("/model") and "\\n" in l for l in hist.splitlines()), hist)
print("pty: %d checks, %d failures" % (checks, fails))
sys.exit(1 if fails else 0)
