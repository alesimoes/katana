/* ---------- uioe/conn_memory.c: o conector memory.recall (docs/uioe.md 3.8.2 e 8.1b item 6, fase U8) ----------
 * Consulta a memoria do dono (~/.katana/memoria.txt, memory_query de agent/memory.c: BM25 com recencia) e devolve cada fato VIVO como um no UN_FACT:
 *   name = o id do fato (8 hex, o mesmo de `katana memory`), sig = a data, text = o fato, cls = "memory".
 * Decisao do dono (2026-11-23): a memoria entra como evidencia DATADA e de BAIXA CONFIANCA, rotulada como lembranca do dono, nunca como fato conferido no codigo.
 *   - confianca: 60 para o fato de hoje, caindo 1 ponto a cada 60 dias, minimo 30; nunca verificado (ver fica UV_NONE: o Construtor escreve `(guess)`).
 *   - faixa 4 (a mais baixa): na ordem final a memoria nunca passa na frente de um item de codigo ou de arquivo relido.
 *   - segredo: memory_query omite o fato que parece segredo e uioe_call ainda mascara o texto de todo no (uioe_mask_line); o texto de um segredo nunca entra.
 *   - nao tem chave de desligar: quem decide se roda e o roteador (pista de historico ou de preferencia no pedido).
 * Fato que cita um arquivo ou simbolo NAO e conferido no indice (o documento previa "names foo.c, which exists"): nao feito nesta fase. */
#define CM_MAX 6
static int cm_recall(UioeCtx *cx, const UioeCall *q, UioeResult *r) {
    (void)cx; snprintf(r->source, sizeof r->source, "memory.recall");
    if (!q->arg[0]) { r->status = UR_EMPTY; snprintf(r->notfound, sizeof r->notfound, "the request has no informative term to look up in memory"); return r->status; }
    MemRec rec[16]; int nsec = 0; const int lim = q->limit > 0 && q->limit < CM_MAX ? q->limit : CM_MAX;
    const int n = memory_query(q->arg, lim, rec, &nsec);
    const long today = (long)(time(NULL) / 86400);
    for (int i = 0; i < n; i++) {
        UioeNode *x = ur_node(r); if (!x) break;
        x->kind = UN_FACT; snprintf(x->name, sizeof x->name, "%s", rec[i].id); snprintf(x->sig, sizeof x->sig, "%.40s", rec[i].date); snprintf(x->cls, sizeof x->cls, "memory");
        ur_set_text(x, rec[i].fact);
        const long dd = mem_days(rec[i].date); const long age = dd >= 0 && today > dd ? today - dd : 0;
        x->conf = (int)(60 - age / 60); if (x->conf < 30) x->conf = 30;
        x->tier = 4; x->step = 3; x->relevance = 30; x->ver = UV_NONE;
        snprintf(x->how, sizeof x->how, "saved note of %.12s (%ld day(s) old), the owner's memory: not verified against the code", rec[i].date, age);
    }
    if (nsec) snprintf(r->limits, sizeof r->limits, "memory.recall: %d matching note(s) look like secrets and were left out", nsec);
    r->total = n; r->status = r->nnode ? UR_OK : UR_EMPTY;
    if (!r->nnode) snprintf(r->notfound, sizeof r->notfound, "nothing saved in memory matches");
    return r->status;
}
