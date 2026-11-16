#!/usr/bin/env python3
"""Servidor MCP minimo para testes do katana (Python puro, sem rede externa).

Uso:
  python3 servidor.py            transporte stdio (uma mensagem JSON por linha)
  python3 servidor.py --http P   transporte HTTP streamable em 127.0.0.1:P (POST /mcp)
Ferramentas: soma(a, b) e hora().
"""
import json
import sys
import time

TOOLS = [
    {"name": "soma", "description": "Soma dois numeros a e b e devolve o resultado.",
     "inputSchema": {"type": "object", "properties": {"a": {"type": "number"}, "b": {"type": "number"}},
                     "required": ["a", "b"]}},
    {"name": "hora", "description": "Devolve a data e hora atuais do servidor.",
     "inputSchema": {"type": "object", "properties": {}}},
]


def lidar(msg):
    """Devolve a resposta (dict) ou None para notificacoes."""
    metodo = msg.get("method")
    mid = msg.get("id")
    if mid is None:
        return None
    if metodo == "initialize":
        res = {"protocolVersion": "2025-06-18", "capabilities": {"tools": {}},
               "serverInfo": {"name": "katana-teste", "version": "1.0"}}
    elif metodo == "ping":
        res = {}
    elif metodo == "tools/list":
        res = {"tools": TOOLS}
    elif metodo == "tools/call":
        p = msg.get("params", {})
        a = p.get("arguments", {}) or {}
        if p.get("name") == "soma":
            try:
                v = float(a["a"]) + float(a["b"])
                txt = str(int(v)) if v == int(v) else str(v)
                res = {"content": [{"type": "text", "text": txt}]}
            except (KeyError, ValueError, TypeError):
                res = {"content": [{"type": "text", "text": "argumentos invalidos"}], "isError": True}
        elif p.get("name") == "hora":
            res = {"content": [{"type": "text", "text": time.strftime("%Y-%m-%d %H:%M:%S")}]}
        else:
            return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": "ferramenta desconhecida"}}
    else:
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "metodo desconhecido"}}
    return {"jsonrpc": "2.0", "id": mid, "result": res}


def stdio():
    for linha in sys.stdin:
        linha = linha.strip()
        if not linha:
            continue
        r = lidar(json.loads(linha))
        if r is not None:
            sys.stdout.write(json.dumps(r) + "\n")
            sys.stdout.flush()


def http(porta):
    from http.server import BaseHTTPRequestHandler, HTTPServer

    class H(BaseHTTPRequestHandler):
        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            msg = json.loads(self.rfile.read(n))
            r = lidar(msg)
            if r is None:
                self.send_response(202)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            if msg.get("method") == "tools/call":      # exercita o formato SSE
                corpo = ("event: message\ndata: " + json.dumps(r) + "\n\n").encode()
                tipo = "text/event-stream"
            else:
                corpo = json.dumps(r).encode()
                tipo = "application/json"
            self.send_response(200)
            self.send_header("Content-Type", tipo)
            self.send_header("Mcp-Session-Id", "sessao-teste")
            self.send_header("Content-Length", str(len(corpo)))
            self.end_headers()
            self.wfile.write(corpo)

        def log_message(self, *a):
            pass

    HTTPServer(("127.0.0.1", porta), H).serve_forever()


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--http":
        http(int(sys.argv[2]))
    else:
        stdio()
