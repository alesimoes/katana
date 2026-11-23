# professor de mentira para o teste do katana distill: resolve a tarefa bank do codebench com chamadas fixas (formato da OpenAI)
# uso: python3 tools/traintest/mock_teacher.py PORTA tools/codebench/tasks/bank/solution/bank.py
import json, sys, http.server
SOL = open(sys.argv[2]).read()
def reply(msgs):
    n = sum(1 for m in msgs if m["role"] == "tool")
    assert msgs[0]["role"] == "system" and "<tools>" not in msgs[0]["content"]
    if n == 0: return {"content": "", "reasoning_content": "Read the code first.", "tool_calls": [{"id": "t1", "type": "function", "function": {"name": "Read", "arguments": json.dumps({"file_path": "bank.py"})}}]}
    if n == 1: return {"content": "Now I fix both bugs.", "tool_calls": [{"id": "t2", "type": "function", "function": {"name": "Write", "arguments": json.dumps({"file_path": "bank.py", "content": SOL})}}]}
    if n == 2: return {"content": "", "tool_calls": [{"id": "t3", "type": "function", "function": {"name": "Bash", "arguments": json.dumps({"command": "python3 -c \"import bank; a=bank.Account(); a.deposit(0.1); a.deposit(0.2); print(a.balance)\""})}}]}
    return {"content": "Fixed: exact Decimal balance, and withdrawing the whole balance works."}
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        assert self.headers.get("Authorization") == "Bearer secret-key", self.headers.get("Authorization")
        assert any(t["function"]["name"] == "Edit" for t in body["tools"])
        m = dict(role="assistant", **reply(body["messages"]))
        out = json.dumps({"choices": [{"message": m, "finish_reason": "stop"}], "usage": {"prompt_tokens": 100, "completion_tokens": 20}}).encode()
        self.send_response(200); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(out))); self.end_headers(); self.wfile.write(out)
    def log_message(self, *a): pass
http.server.HTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
