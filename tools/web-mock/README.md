# web-mock

A stand-in for `katana serve --agent`, to look at and click through the embedded web page (SRV_PAGE in
`src/app/server.c`) without a model. It reads the page from the C source, so it always shows the current one.

    python3 tools/web-mock/mock.py [port] [--chat-only] [--key SECRET]

- default: imitates `/v1/agent/sessions` with canned server-sent events (tool cards, live output, plan, question,
  permission, notifications, context meter, exit). Message words choose the scenario: `pergunta`, `permiss`,
  `erro`, `sair`.
- `--chat-only`: behaves like a server without `--agent` (the page falls back to the plain chat).
- `--key SECRET`: `/v1/*` needs `Authorization: Bearer SECRET` (the page asks for the key after a 401).

Nothing here talks to a model; it checks the page's rendering and logic only.
