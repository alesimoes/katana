"""Auxiliares para os setup.py das tarefas (servidor local de paginas, porta livre)."""
import os, shutil, socket, subprocess, sys, tempfile, time, urllib.request


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def start_site(site_dir):
    """copia site_dir para uma pasta temporaria e serve em 127.0.0.1 numa porta livre (python3 -m http.server).
    Devolve (porta, parar); parar() encerra o servidor e apaga a copia."""
    root = tempfile.mkdtemp(prefix="cb_site_")
    shutil.copytree(site_dir, root, dirs_exist_ok=True)
    port = free_port()
    proc = subprocess.Popen([sys.executable, "-m", "http.server", str(port), "--bind", "127.0.0.1"], cwd=root,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def stop():
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        shutil.rmtree(root, ignore_errors=True)

    for _ in range(100):                      # espera o servidor responder
        try:
            urllib.request.urlopen("http://127.0.0.1:%d/" % port, timeout=1).read()
            return port, stop
        except Exception:
            if proc.poll() is not None:
                break
            time.sleep(0.05)
    stop()
    raise RuntimeError("servidor local nao subiu")
