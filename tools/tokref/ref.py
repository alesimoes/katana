#!/usr/bin/env python3
"""Gera a referencia do teste dourado de tokenizacao e de template de conversa.

Uso (da raiz do repositorio):
    PYTHONPATH=tools/tokref/vendor python3 -I tools/tokref/ref.py
(com -I o PYTHONPATH e ignorado: use  python3 -s tools/tokref/ref.py  ou veja run.sh)

Entradas : tools/tokref/dl/<repo>/{tokenizer.json,tokenizer_config.json,...} (baixados por dl.sh)
Saidas   : tools/tokref/out/corpus.hex     um caso por linha, em hexa UTF-8 (entrada do `katana toktest`)
           tools/tokref/out/chat.hex       um caso de conversa por linha: sys user asst user2 em hexa ('-' = vazio)
           tools/tokref/out/<modelo>.ref.json  ids de referencia e texto do template de cada caso
"""
import json, os, sys, base64, glob

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "vendor"))
sys.path.insert(0, HERE)
from tokenizers import Tokenizer, AddedToken, pre_tokenizers, decoders, Regex
from tokenizers.models import BPE
import jinja2
from jinja2.sandbox import ImmutableSandboxedEnvironment
from jinja2.ext import Extension
import ggufmeta

DL = os.path.join(HERE, "dl")
OUT = os.path.join(HERE, "out")
MODELOS = "/mnt/dados/source/katana/models"

# modelo katana -> (pasta em dl/, gguf)
MODELS = {
    "qwen2.5": ("Qwen2.5-1.5B-Instruct", "qwen2.5-1.5b-q4km.gguf"),
    "qwen3-30b": ("Qwen3-30B-A3B-Instruct-2507", "Qwen3-30B-A3B-Instruct-2507-Q4_K_M.gguf"),
    "qwen3.6": ("Qwen3.6-35B-A3B", "Qwen3.6-35B-A3B-UD-Q4_K_M.gguf"),
    "deepseek-v2-lite": ("DeepSeek-V2-Lite-Chat", "deepseek-v2-lite-chat-q4k.gguf"),
    "moonlight": ("Moonlight-16B-A3B-Instruct", "moonlight-16b-a3b-instruct-q4km.gguf"),
    "llama3.2": (None, "llama3.2-3b-q4km.gguf"),   # sem referencia publica aberta: autoteste com o vocabulario do proprio GGUF
    "olmoe": ("OLMoE-1B-7B-0924-Instruct", "olmoe-1b-7b-q4km.gguf"),   # o GGUF e o 0924 (o 0125 troca os ids dos tokens especiais)
}

# ---------------------------------------------------------------- corpus
CORPUS = [
    "Hello world",
    "Ola, mundo!",
    "A raposa marrom rapida pulou sobre o cachorro preguicoso.",
    "Ação, coração, não, índice, ônibus, açúcar, pingüim, café com leite às 15h.",
    "O Brasil é um país de dimensões continentais; São Paulo tem mais de 12 milhões de habitantes.",
    "The quick brown fox jumps over the lazy dog. It's a dog's life, isn't it? They're here, we've won, I'll go, I'm fine, he'd say.",
    "IT'S THEY'RE WE'VE I'LL I'M HE'D SHE'S DON'T",
    "don't  can't  won't 'tis 'twas rock'n'roll",
    "int main(int argc, char **argv) {\n    printf(\"hello, %s\\n\", argv[0]);\n    return 0;\n}\n",
    "static inline float dot(const float *a, const float *b, size_t n) {\n\tfloat s = 0;\n\tfor (size_t i = 0; i < n; i++) s += a[i] * b[i];\n\treturn s;\n}",
    "def fib(n):\n    if n < 2:\n        return n\n    return fib(n - 1) + fib(n - 2)\n\n\nprint([fib(i) for i in range(10)])\n",
    "#include <stdio.h>\n#define MAX(a,b) ((a)>(b)?(a):(b))\n/* comentario */\n// outro\n",
    "x = {'a': [1, 2, 3], \"b\": None, 'c': lambda y: y**2 + 3.14159}",
    "12345678901234567890",
    "3.14159265358979323846264338327950288",
    "0123456789 10 100 1000 10000 100000 1000000",
    "R$ 1.234.567,89 em 2024-11-16 às 12:34:56",
    "1 2 3 4 5 6 7 8 9 0",
    "a1b22c333d4444e55555f666666",
    "v1.2.3-rc4+build.567",
    "Emoji: \U0001F600 \U0001F680 \U0001F1E7\U0001F1F7 \u2764\ufe0f \U0001F468\u200d\U0001F469\u200d\U0001F467 \U0001F44D\U0001F3FD",
    "\U0001F600\U0001F600\U0001F600",
    "ok \U0001F600!",
    "你好，世界！这是一个测试。我们今天学习自然语言处理。",
    "日本語のテキスト、ひらがな、カタカナ、漢字を含む文章です。",
    "한국어 문장입니다. 안녕하세요?",
    "Привет, мир! Это тест токенизатора.",
    "مرحبا بالعالم",
    "שלום עולם",
    "ελληνικά κείμενο και ΚΕΦΑΛΑΙΑ",
    "हिन्दी पाठ परीक्षण",
    "ไทยทดสอบ",
    "Mixed 中文English混合mixed日本語text한국어end",
    "Ünïcödé: ÀÉÎÕÜ àéîõü ÿ ß ñ Ñ ǅ ǆ ǈ",
    "e\u0301 a\u0300 n\u0303 (acentos decompostos) c\u0327",
    "caf\u00e9 vs cafe\u0301",
    "a  b   c    d",
    "  leading spaces",
    "trailing spaces   ",
    "     ",
    " ",
    "\n",
    "\n\n",
    "\n\n\n\n",
    "linha1\nlinha2\n\nlinha4\n\n\nlinha7",
    "linha1\r\nlinha2\r\n\r\nlinha4\r\n",
    "tab\tseparado\t\tduplo",
    "  \n  \n  ",
    "texto   \n   mais texto",
    "end \n",
    "word \n\n word",
    "foo\n    bar\n        baz\n",
    "                                                  ",
    "a" + " " * 30 + "b",
    "x\u00a0y\u2003z\u3000w",
    "linha\u2028sep\u2029par",
    "!!! ??? ... --- === *** ###",
    "(a)(b)[c]{d}<e>",
    "https://www.example.com/path/to/page?query=1&other=2#frag",
    "user@example.com, +55 (11) 91234-5678",
    "{\"name\": \"Alice\", \"age\": 30, \"tags\": [\"a\", \"b\"], \"nested\": {\"k\": null}}",
    "<html><body><div class=\"x\">oi</div></body></html>",
    "SELECT * FROM users WHERE id = 42 AND name LIKE '%joao%';",
    "a+b=c; x*y/z - 1 % 2 ^ 3 | 4 & 5 ~6",
    "```python\nprint('oi')\n```",
    "# Titulo\n\n- item 1\n- item 2\n\n**negrito** e *italico*\n",
    "_ __ ___ _foo foo_ __init__",
    "\u2018aspas\u2019 \u201cduplas\u201d \u2014 travessao \u2013 meio \u2026 reticencias",
    "\u00ab francês \u00bb \u00bfEspañol? \u00a1Sí!",
    "$$ \u20ac \u00a3 \u00a5 \u20b9 \u00a9 \u00ae \u2122 \u00b0 \u00b1 \u00d7 \u00f7",
    "\u2200x \u2208 \u211d: x\u00b2 \u2265 0, \u03b1+\u03b2=\u03b3, \u221a2 \u2248 1.414, \u222b f dx",
    "\u0661\u0662\u0663 \u0967\u0968\u0969 \u2460\u2461\u2462 \u00bd \u00b2\u00b3 \u2167",
    "\u0e01\u0e32\u0e23\u0e17\u0e14\u0e2a\u0e2d\u0e1a",
    "\x01\x02 controle \x7f del \x1b[0m",   # NUL fica de fora: o motor trabalha com strings C
    "bytes invalidos: \udcff\udcfe".encode("utf-8", "surrogateescape").decode("latin-1") if False else "latin1: \u00ff\u00fe",
    "<|im_start|>user\nOla<|im_end|>\n<|im_start|>assistant\n",
    "<|endoftext|>",
    "texto<|endoftext|>mais texto",
    "<|im_end|>",
    "<|im_start|>system\nVoce e util.<|im_end|>",
    "<think>\nraciocinio\n</think>\n\nresposta",
    "</think>",
    "<tool_call>\n{\"name\": \"f\", \"arguments\": {}}\n</tool_call>",
    "<tool_response>x</tool_response>",
    "<|fim_prefix|>def f():<|fim_suffix|>\n<|fim_middle|>",
    "<|vision_start|><|image_pad|><|vision_end|>",
    "<\uff5cbegin\u2581of\u2581sentence\uff5c>texto<\uff5cend\u2581of\u2581sentence\uff5c>",
    "<\uff5cbegin\u2581of\u2581sentence\uff5c>User: oi\n\nAssistant:",
    "<|user|>\nOla\n<|assistant|>\n",
    "<|system|>\nS\n",
    "|||IP_ADDRESS|||",
    "<|padding|>",
    "<|im_user|>user<|im_middle|>oi<|im_end|>",
    "<|im_assistant|>assistant<|im_middle|>",
    "<|im_system|>system<|im_middle|>x<|im_end|>",
    "[BOS][EOS]",
    "<s> </s> <unk> <pad>",
    "<|begin_of_text|><|start_header_id|>user<|end_header_id|>\n\noi<|eot_id|>",
    "<|im_end|>\n<|im_start|>",
    "<<|im_start|>>",
    "<|im_start",
    "<|notatoken|>",
    "< | im_start | >",
    "<",
    "<<<",
    "a<b>c",
    "x<|im_end|>y<|im_end|>z",
    "<|endoftext|><|endoftext|>",
    "<|endoftext|>\n",
    "\n<|endoftext|>",
    " <|endoftext|>",
    "texto longo " * 50,
    "abcdefghijklmnopqrstuvwxyz" * 8,
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "camelCaseIdentifier PascalCaseIdentifier snake_case_identifier kebab-case-identifier",
    "HTTPServerError XMLHttpRequest getHTTPResponseCode",
    "McDonald's iPhone eBay LaTeX",
    "Ǆ Ǉ ǋ ᾈ ᾉ",
    "\u0391\u03b2\u03b3 \u03a9\u03c9",
    "tab\u200bzero\u200dwidth\u200ejoiner\ufeffbom",
    "\u3042\u3044\u3046 \u30a2\u30a4\u30a6 \u4e00\u4e8c\u4e09 \u3000\u3000",
    "\u4e2d\u6587abc\u4e2d\u6587 123 \u4e2d\u6587",
    "\u3002\uff0c\u3001\uff01\uff1f\uff1a\uff1b\u300c\u300d\u300e\u300f",
    "\uff21\uff22\uff23\uff11\uff12\uff13 \uff01\uff1f",
    "O gato.\n\nO cachorro!\n\n\nO passaro?",
    "Texto com    muitos     espacos    internos e linhas\n\n\n  indentadas.",
    "Q: Quanto é 17 * 23?\nA: 391",
    "1,000,000.50 e -42 e +7 e 1e10 e 0x1F e 0b101",
    "2^10 = 1024; 10^-3 = 0.001",
    "'", "''", "'s", "'S", "'re", "'RE", "'ve", "'ll", "'d", "'m", "'t", "x's", "X'S", "a'b", "'abc",
    ". . .", ".\n", ".\n\n", "?\n", "word.\n\nWord", "a\n b", "a \nb", "a\n\n b",
    "\t", "\t\t", " \t ", "\r", "\r\n", "\r\r\n", "a\rb", "a\r\n\r\nb",
    "é", "é é", "ée", " é", "1é", "é1", "_é", "-é", ":é",
    "Hello,World", "Hello, World", "Hello ,World", "Hello , World",
]

# conversas: (sistema, usuario, assistente, usuario2) ; None = ausente
CHATS = [
    ("Voce e um assistente util.", "Qual a capital da Franca?", "A capital da Franca e Paris.", "E da Italia?"),
    (None, "Qual a capital da Franca?", "A capital da Franca e Paris.", "E da Italia?"),
    ("You are a pirate.", "Say hi.", "Arr, hi!", "Again, with feeling."),
    ("Responda em portugues com acentos: ação, coração.", "Explique o que é um tokenizador.\n\nSeja breve.", "Um tokenizador divide texto em tokens.", "E o BPE?"),
    ("Sistema com\nquebras de linha\n\ne espacos  duplos.", "  usuario com espacos nas pontas  ", "resposta", "mais?"),
    (None, "Escreva uma função em C que soma dois inteiros:\n\nint soma(int a, int b)", "```c\nint soma(int a, int b) { return a + b; }\n```", "Agora em Python."),
    ("Seja formal.", "oi", "Saudacoes.", "tudo bem?"),
    (None, "你好，请介绍一下你自己。 \U0001F600", "我是一个语言模型。", "谢谢"),
]

def extra_corpus():
    """casos extras: texto real do repositorio (docs em portugues, codigo C) e texto aleatorio de fragmentos de varias classes"""
    import random
    root = os.path.abspath(os.path.join(HERE, "..", ".."))
    out = []
    for arq, passo, n in (("docs/knowledge.md", 400, 40), ("docs/handoff.md", 700, 40), ("docs/plan.md", 300, 25),
                          ("docs/base-ia/nlp-e-llm.md", 900, 40), ("katana.c", 800, 60), ("tools/tokref/ref.py", 500, 20)):
        p = os.path.join(root, arq)
        if not os.path.exists(p):
            continue
        t = open(p, encoding="utf-8", errors="replace").read()
        for i in range(n):
            a = (i * (len(t) // max(1, n)))
            out.append(t[a:a + passo].replace("\x00", ""))
    rng = random.Random(1234)
    frag = ["a", "b", "Z", "é", "ç", "ñ", "ß", "Ǆ", "ǅ", "ᾈ", "Æ", "1", "7", "0", "٣", "²", "½", "Ⅷ", " ", " ", " ", "  ", "\n", "\r", "\r\n",
            "\t", "\u00a0", "\u3000", "\u2003", "\u2028", ".", ",", "!", "?", "-", "_", "'", "'s", "'S", "'re", "'LL", "'d", "\"", "(", ")", "/",
            "\\", "<", ">", "|", "*", "#", "$", "@", "中", "文", "字", "あ", "ア", "한", "글", "я", "Ж", "α", "Ω", "א", "ع", "ह", "ก",
            "\U0001F600", "\U0001F44D", "\u200d", "\ufe0f", "\u0301", "\u0308", "\u0327", "\u0323", "\u0345", "\u1100", "\u1161", "\u11a8",
            "\u2018", "\u2019", "\u201c", "…", "—", "€", "™", "→", "∀", "<|im_end|>", "<|endoftext|>", "<think>", "foo", "Hello", "WORLD", "ção", "123456"]
    for _ in range(1500):
        out.append("".join(rng.choice(frag) for _ in range(rng.randint(1, 25))))
    return out


CORPUS += extra_corpus()

SPECIAL_TEMPLATE_VARS = {}


class GenerationExt(Extension):
    """aceita {% generation %}...{% endgeneration %} (so marca regioes do assistente) como bloco transparente"""
    tags = {"generation"}

    def parse(self, parser):
        lineno = next(parser.stream).lineno
        body = parser.parse_statements(["name:endgeneration"], drop_needle=True)
        return jinja2.nodes.CallBlock(self.call_method("_pass", []), [], [], body).set_lineno(lineno)

    def _pass(self, caller):
        return caller()


def make_env():
    env = ImmutableSandboxedEnvironment(trim_blocks=True, lstrip_blocks=True, extensions=[GenerationExt])

    def raise_exception(msg):
        raise jinja2.exceptions.TemplateError(msg)

    env.globals["raise_exception"] = raise_exception
    env.globals["strftime_now"] = lambda fmt: "26 Jul 2024"
    env.filters["tojson"] = lambda x, **kw: json.dumps(x, ensure_ascii=False)
    return env


def render(tmpl, msgs, bos, eos):
    env = make_env()
    t = env.from_string(tmpl)
    return t.render(messages=msgs, add_generation_prompt=True, bos_token=bos or "", eos_token=eos or "")


def tok_str(v):
    if isinstance(v, dict):
        return v.get("content")
    return v


# ---------------------------------------------------------------- tokenizadores
def bytes_to_unicode():
    bs = list(range(ord("!"), ord("~") + 1)) + list(range(ord("¡"), ord("¬") + 1)) + list(range(ord("®"), ord("ÿ") + 1))
    cs = bs[:]
    n = 0
    for b in range(256):
        if b not in bs:
            bs.append(b); cs.append(256 + n); n += 1
    return dict(zip(bs, [chr(c) for c in cs]))


def moonlight_tokenizer(d):
    """Moonlight so publica tiktoken.model: reconstroi um BPE equivalente (conversao padrao tiktoken -> BPE)
    com o regex oficial de tokenization_moonshot.py (a biblioteca tokenizers aceita a sintaxe && do regex)."""
    ranks = {}
    for line in open(os.path.join(d, "tiktoken.model"), "rb"):
        if line.strip():
            a, b = line.split()
            ranks[base64.b64decode(a)] = int(b)
    b2u = bytes_to_unicode()
    enc = lambda bs: "".join(b2u[x] for x in bs)

    def bpe(token, max_rank):
        parts = [bytes([b]) for b in token]
        while True:
            mi = None; mr = None
            for i in range(len(parts) - 1):
                r = ranks.get(parts[i] + parts[i + 1])
                if r is not None and (mr is None or r < mr):
                    mi, mr = i, r
            if mr is None or mr >= max_rank:
                break
            parts = parts[:mi] + [parts[mi] + parts[mi + 1]] + parts[mi + 2:]
        return parts

    vocab = {enc(t): r for t, r in ranks.items()}
    merges = []
    for t, r in sorted(ranks.items(), key=lambda x: x[1]):
        if len(t) == 1:
            continue
        p = bpe(t, r)
        if len(p) == 2:
            merges.append((enc(p[0]), enc(p[1])))
    cfg = json.load(open(os.path.join(d, "tokenizer_config.json")))
    for k, v in cfg.get("added_tokens_decoder", {}).items():
        vocab[v["content"]] = int(k)       # especiais nos ids oficiais (senao a biblioteca os numera em sequencia)
    tk = Tokenizer(BPE(vocab=vocab, merges=merges, ignore_merges=True))
    src = open(os.path.join(d, "tokenization_moonshot.py"), encoding="utf-8").read()
    ns = {}
    # extrai pat_str do arquivo oficial sem importar o modulo (precisa de tiktoken)
    i = src.index("pat_str = ")
    j = src.index("\n    )\n", i) + 7
    exec("class _C:\n    " + src[i:j].replace("\n    ", "\n    "), ns)
    pat = ns["_C"].pat_str
    tk.pre_tokenizer = pre_tokenizers.Sequence([
        pre_tokenizers.Split(Regex(pat), "isolated", False),
        pre_tokenizers.ByteLevel(add_prefix_space=False, use_regex=False)])
    toks = []
    for k, v in cfg.get("added_tokens_decoder", {}).items():
        toks.append(AddedToken(v["content"], special=v.get("special", True), normalized=False))
    tk.add_special_tokens(toks)
    return tk


def llama3_from_gguf(kv):
    """Llama 3 (tokenizer.json oficial e gated): monta o BPE da biblioteca com o vocabulario e as fusoes do proprio GGUF e o regex
    publicado do Llama 3. Valida o pre-tokenizador e os especiais do katana contra o motor de regex da biblioteca, nao o vocabulario."""
    toks = kv["tokenizer.ggml.tokens"]; ty = kv["tokenizer.ggml.token_type"]
    vocab = {t: i for i, t in enumerate(toks)}
    merges = [tuple(m.split(" ")) for m in kv["tokenizer.ggml.merges"]]
    tk = Tokenizer(BPE(vocab=vocab, merges=merges, ignore_merges=True))
    pat = r"(?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\r\n\p{L}\p{N}]?\p{L}+|\p{N}{1,3}| ?[^\s\p{L}\p{N}]+[\r\n]*|\s*[\r\n]+|\s+(?!\S)|\s+"
    tk.pre_tokenizer = pre_tokenizers.Sequence([pre_tokenizers.Split(Regex(pat), "isolated", False),
                                                pre_tokenizers.ByteLevel(add_prefix_space=False, use_regex=False)])
    tk.add_special_tokens([AddedToken(toks[i], special=True, normalized=False) for i in range(len(toks)) if ty[i] == 3])
    return tk


def load_tok(d):
    p = os.path.join(d, "tokenizer.json")
    if os.path.getsize(p) > 100:
        return Tokenizer.from_file(p), "tokenizer.json oficial"
    return moonlight_tokenizer(d), "reconstruido de tiktoken.model + regex oficial"


def main():
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, "corpus.hex"), "w") as f:
        for s in CORPUS:
            f.write(s.encode("utf-8", "surrogatepass").hex() + "\n")
    with open(os.path.join(OUT, "chat.hex"), "w") as f:
        for c in CHATS:
            f.write(" ".join(("-" if x is None else ("~" if x == "" else x.encode().hex())) for x in c) + "\n")
    only = sys.argv[1:]
    for name, (dd, gg) in MODELS.items():
        if only and name not in only:
            continue
        gg_path = os.path.join(MODELOS, gg)
        kv = ggufmeta.meta(gg_path) if os.path.exists(gg_path) else {}
        if dd is None:
            if not kv: continue
            tk, how = llama3_from_gguf(kv), "autoteste: BPE da biblioteca com o vocabulario do GGUF e o regex do Llama 3"
            cfg = {}; hf_tmpl = None
        else:
            d = os.path.join(DL, dd)
            if not os.path.isdir(d):
                print("pulado (sem arquivos):", name); continue
            tk, how = load_tok(d)
            cfg = json.load(open(os.path.join(d, "tokenizer_config.json")))
            hf_tmpl = cfg.get("chat_template")
            jp = os.path.join(d, "chat_template.jinja")
            if os.path.exists(jp):
                hf_tmpl = open(jp, encoding="utf-8").read()
        gg_tmpl = kv.get("tokenizer.chat_template")
        bos = tok_str(cfg.get("bos_token")); eos = tok_str(cfg.get("eos_token"))
        # BOS/EOS do proprio GGUF (o que o modelo testado realmente usa)
        toks = kv.get("tokenizer.ggml.tokens")
        if toks:
            if kv.get("tokenizer.ggml.bos_token_id") is not None: bos = toks[kv["tokenizer.ggml.bos_token_id"]]
            if kv.get("tokenizer.ggml.eos_token_id") is not None: eos = toks[kv["tokenizer.ggml.eos_token_id"]]
        res = {"model": name, "tokenizer": how, "bos": bos, "eos": eos, "tok": [], "chat": [], "chat_hf": [],
               "template_gguf_igual_hf": (hf_tmpl == gg_tmpl), "think": bool(gg_tmpl and "<think>" in gg_tmpl), "ref_tmpl": "gguf" if gg_tmpl else "hf"}
        for s in CORPUS:
            try:
                res["tok"].append(tk.encode(s, add_special_tokens=False).ids)
            except Exception as e:
                res["tok"].append(None)
        for (sy, u1, a1, u2) in CHATS:
            def build(use_sys, turns):
                m = []
                if use_sys and sy is not None:
                    m.append({"role": "system", "content": sy})
                return m
            # variante 1: [sys] user (1 turno); variante 2: [sys] user asst user2 (2 turnos)
            base = [] if sy is None else [{"role": "system", "content": sy}]
            one = base + [{"role": "user", "content": u1}]
            two = one + [{"role": "assistant", "content": a1}, {"role": "user", "content": u2}]
            row = {}
            row_hf = {}
            for k, ms in (("t1", one), ("t2", two)):
                for tm, dst in ((gg_tmpl, row), (hf_tmpl, row_hf)):
                    if not tm:
                        dst[k] = None; continue
                    try:
                        dst[k] = render(tm, ms, bos, eos)
                    except Exception as e:
                        dst[k] = "ERRO: %s" % e
            res["chat"].append({k: (v, tk.encode(v, add_special_tokens=False).ids if v and not v.startswith("ERRO") else None) for k, v in row.items()})
            res["chat_hf"].append(row_hf)
        json.dump(res, open(os.path.join(OUT, name + ".ref.json"), "w"), ensure_ascii=False)
        print("ok", name, how, "| template gguf==hf:", res["template_gguf_igual_hf"], "| casos:", len(CORPUS), len(CHATS))


if __name__ == "__main__":
    main()
