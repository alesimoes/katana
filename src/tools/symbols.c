/* ---------- ferramenta Symbols (ver docs/tools.md) ----------
 * As consultas estruturais do indice de simbolos (`katana coder def|refs|callers|callees|uses|outline|at|deps|impact|flow`) para o modelo,
 * numa so ferramenta com `action` (uma declaracao no lugar de dez). Nao esta em core[] do prompt: aparece na lista "por nome" e o
 * modelo a pega com ToolSearch. O trabalho esta em src/coder/symtool.c; aqui ficam o esquema e a leitura dos argumentos. */
static const char CT_SYMBOLS_DESC[] =
    "Structural code queries from the symbol index of the working folder (functions, types, calls; built on first use, then kept up to date). "
    "Exact where Find is fuzzy: use it to follow code by name. `action`: "
    "def = where `name` is defined (file:lines, signature); "
    "refs = every place `name` appears, by kind, each with the function it is in; "
    "callers = who calls `name`; callees = what `name` calls, in call order; "
    "uses = occurrences of `name` that are not calls (reads, types, assignments); "
    "outline = the symbols of `file`, nested, with line ranges (read this instead of the whole file); "
    "tree = the folders of the project (or of folder `file`), `depth` levels deep: files per language, how deeply each is indexed, what it is for, its main symbols, and the folders that are ignored and why; "
    "at = the symbol and its enclosing symbols at `file`:`line` (which function is line 120 in?); "
    "deps = what `file` imports and which files import it; "
    "impact = the callers of `name`, their callers and so on (what a change can break; `depth` default 3); "
    "flow = links that go through TEXT instead of calls: who sends/emits/writes/calls a text key and who handles/reads it (message type values like \"register\", "
    "event names, HTTP routes like /channel, environment variables, config keys, subcommands). `name` = the key, or `file` (+ `line`) = the keys that file or "
    "function sends and receives and where they go; with neither, a summary of the project's file-to-file links by key. Use it when two files talk to each other "
    "without importing or calling each other (WebSocket and HTTP between processes, events, env vars, CLI subcommands). "
    "Each result is file:lines, the kind, and [confidence N level; how it was found]. If the exact name does not exist it tries other spellings "
    "and says so; when nothing matches it answers NOT FOUND and states what was indexed (macro-generated names and calls through pointers are not seen). "
    "`limit` caps the results. `path` is a project folder other than the working folder. Then Read only the lines it points to.";
static const char CT_SYMBOLS_PROPS[] =
    "\"action\": {\"type\": \"string\", \"enum\": [\"def\", \"refs\", \"callers\", \"callees\", \"uses\", \"outline\", \"at\", \"deps\", \"impact\", \"flow\", \"tree\"], \"description\": \"The query\"}, "
    "\"name\": {\"type\": \"string\", \"description\": \"Symbol name, for def, refs, callers, callees, uses, impact; the text key for flow (register, /channel, KATANA_SESSION_ID)\"}, "
    "\"file\": {\"type\": \"string\", \"description\": \"File, for outline, at, deps, flow; the folder to show for tree\"}, "
    "\"line\": {\"type\": \"integer\", \"description\": \"Line number, for at and flow\"}, "
    "\"limit\": {\"type\": \"integer\", \"description\": \"Maximum results (default depends on the action, max 100; tree: folder lines, default 40)\"}, "
    "\"depth\": {\"type\": \"integer\", \"description\": \"impact: levels of callers (default 3, max 6); tree: levels of folders (default 2, max 4)\"}, "
    "\"path\": {\"type\": \"string\", \"description\": \"Project folder (default: the working folder)\"}";
static const char CT_SYMBOLS_REQ[] = "\"action\"";

#ifndef _WIN32
static int sym_tool_run(const char *action, const char *name, const char *file, long long line, long long limit, long long depth, const char *root, char *out, size_t cap);   /* coder/symtool.c */
static void ct_symbols(const char *js, const char *end, char *out, size_t cap) {
    if (cap < 300) { if (cap) out[0] = 0; return; }
    char *action = ct_str(js, end, "action"), *name = ct_str(js, end, "name"), *file = ct_str(js, end, "file"), *path = ct_str(js, end, "path");
    if (!name || !*name) { free(name); name = ct_str(js, end, "query"); }
    if (!file || !*file) { free(file); file = ct_str(js, end, "file_path"); }
    const long long line = ct_int(js, end, "line", 0), limit = ct_int(js, end, "limit", 0), depth = ct_int(js, end, "depth", 0);
    if (!action || !*action) snprintf(out, cap, "error: missing `action` (def, refs, callers, callees, uses, outline, at, deps, impact, flow, tree). Example: {\"action\": \"def\", \"name\": \"parseConfig\"}");
    else if ((!path || !*path) && ct_cwd_is_home())
        snprintf(out, cap, "error: the working folder is your home folder, not a project, so there is no symbol index to build here. "
                 "Set `path` to the project folder the user means (if the user named it, find where it is with Bash: find / -maxdepth 4 -type d -name NAME 2>/dev/null), then call Symbols again with that path.");
    else {
        char cwd[4096], res[4200]; struct stat st;
        if (!getcwd(cwd, sizeof cwd)) snprintf(cwd, sizeof cwd, ".");
        if (path && *path) { if (!ct_dir_arg(path, 0, res, sizeof res, &st, out, cap)) goto done; if (!S_ISDIR(st.st_mode)) { snprintf(out, cap, "error: path must be a folder"); goto done; } }
        else snprintf(res, sizeof res, "%s", cwd);
        sym_tool_run(action, name, file, line, limit, depth, res, out, cap);
    }
done:
    free(action); free(name); free(file); free(path);
}
#else
static void ct_symbols(const char *js, const char *end, char *out, size_t cap) { (void)js; (void)end; snprintf(out, cap, "error: not available on Windows yet"); }
#endif
