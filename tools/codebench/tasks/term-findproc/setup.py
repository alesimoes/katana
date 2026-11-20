import os, shutil, socket, subprocess, sys, tempfile, time
from bench_lib import free_port

# um servico que escuta na porta (a porta vem de um arquivo, nao da linha de comando), um servico de outra porta e
# um processo isca que so cita a porta na linha de comando

SERVER = """import socket, sys, time
port = int(open("port.cfg").read())
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", port))
s.listen(5)
while True:
    time.sleep(1)
"""


def setup(work):
    base = os.path.realpath(tempfile.mkdtemp(prefix="cb_proc_"))
    procs = []

    def start(subdir, script, port, args=()):
        d = os.path.join(base, subdir)
        os.makedirs(d)
        with open(os.path.join(d, "svc.py"), "w") as f:
            f.write(script)
        with open(os.path.join(d, "port.cfg"), "w") as f:
            f.write(str(port))
        p = subprocess.Popen([sys.executable, "svc.py"] + list(args), cwd=d, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
        procs.append(p)
        return p, d

    def stop():
        for p in procs:
            p.terminate()
        for p in procs:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        shutil.rmtree(base, ignore_errors=True)

    try:
        port, other = free_port(), free_port()
        while other == port:
            other = free_port()
        real, cwd = start("releases/2026-11-04/api", SERVER, port)
        start("releases/2026-10-30/api", SERVER, other)
        start("tmp/sleeper", "import time\ntime.sleep(3600)\n", 0, ["--port=%d" % port, "--listen", str(port)])
        for p_ in (port, other):                  # espera os dois servicos escutarem
            for _ in range(100):
                try:
                    socket.create_connection(("127.0.0.1", p_), timeout=1).close()
                    break
                except OSError:
                    time.sleep(0.05)
    except Exception:
        stop()
        raise
    return dict(vars={"port": port, "pid": real.pid, "cwd": cwd}, stop=stop)
