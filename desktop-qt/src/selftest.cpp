// Selftest de ponta a ponta: sobe o agente com o modelo pequeno, digita e envia pela interface
// (eventos de teclado e mouse reais na janela QML), confere cartoes de ferramenta, sessoes,
// dropdown, apagar com confirmacao, parar com SIGINT, e grava capturas PNG da janela.
#include "selftest.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QtTest/QTest>
#include <functional>

#include "backend.h"
#include "sessoes.h"

namespace {

int g_fail = 0, g_ok = 0;

void typeText(QWindow *w, const QString &t)
{
    for (QChar ch : t)
        QTest::keyClick(w, ch.toLatin1());
}

void check(bool ok, const QString &what)
{
    (ok ? g_ok : g_fail)++;
    fprintf(stderr, "  [%s] %s\n", ok ? "ok" : "FALHA", qPrintable(what));
}

bool waitFor(const std::function<bool()> &cond, int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        if (cond())
            return true;
        QTest::qWait(50);
    }
    return cond();
}

QQuickItem *findItem(QQuickItem *root, const QString &name)
{
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem *c : root->childItems())
        if (QQuickItem *r = findItem(c, name))
            return r;
    return nullptr;
}

void findAll(QQuickItem *root, const QString &name, QList<QQuickItem *> &out)
{
    if (!root)
        return;
    if (root->objectName() == name)
        out.append(root);
    for (QQuickItem *c : root->childItems())
        findAll(c, name, out);
}

QPoint center(QQuickItem *it) { return it->mapToScene(QPointF(it->width() / 2, it->height() / 2)).toPoint(); }

struct Ctx {
    QQuickWindow *win;
    Backend *be;
    QString capDir;
    QQuickItem *root() const { return win->contentItem(); }
    QQuickItem *item(const QString &n) const { return findItem(root(), n); }
    void click(QQuickItem *it)
    {
        QTest::mouseMove(win, center(it));
        QTest::mouseClick(win, Qt::LeftButton, Qt::NoModifier, center(it));
        QTest::qWait(120);
    }
    void grab(const QString &name)
    {
        if (capDir.isEmpty())
            return;
        QTest::qWait(400);
        QImage img = win->grabWindow();
        if (img.width() > 1000)
            img = img.scaledToWidth(1000, Qt::SmoothTransformation);
        QDir().mkpath(capDir);
        img.save(QDir(capDir).filePath(name));
        fprintf(stderr, "  captura: %s (%dx%d)\n", qPrintable(name), img.width(), img.height());
    }
    void typeAndEnter(const QString &text)
    {
        QQuickItem *in = item("composerInput");
        click(in);
        typeText(win, text);
        QTest::keyClick(win, Qt::Key_Return);
    }
    QList<QQuickItem *> sessionItems() const
    {
        QList<QQuickItem *> l;
        findAll(root(), "sessionItem", l);
        return l;
    }
    QQuickItem *sessionItemWithTitle(const QString &t) const
    {
        for (QQuickItem *s : sessionItems())
            if (s->property("modelData").toMap()["titulo"].toString() == t)
                return s;
        return nullptr;
    }
    bool popupVisible() const
    {
        QQuickItem *cb = item("modelCombo");
        QObject *p = cb ? cb->property("popup").value<QObject *>() : nullptr;
        return p && p->property("visible").toBool();
    }
    QString inputText() const { return item("composerInput")->property("text").toString(); }
};

void fakeSession(const QString &dir, const QString &id, const QString &titulo, const QString &modelo, qint64 agoSec,
                 const QList<Msg> &ms)
{
    Session s;
    s.id = id;
    s.titulo = titulo;
    s.modelo = modelo;
    QDateTime t = QDateTime::currentDateTime().addSecs(-agoSec);
    t = t.toOffsetFromUtc(t.offsetFromUtc());
    s.criada = s.atualizada = t.toString(Qt::ISODate);
    s.mensagens = ms;
    Sessoes::salvar(dir, s);
}

}  // namespace

int runSelfTest(Backend &be, QQuickWindow *win, const QString &capDir, const QString &model)
{
    Ctx c{win, &be, capDir};
    const QString dir = be.sessionsDir();
    const QString modelo = "Qwen3.6-35B-A3B-UD-Q4_K_M.gguf";
    fprintf(stderr, "== selftest: modelo %s, dados %s\n", qPrintable(model), qPrintable(dir));
    win->resize(1180, 820);
    win->show();
    QTest::qWait(500);

    // ---- modelos ----
    fprintf(stderr, "-- modelos\n");
    QVariantMap small, q36;
    for (const QVariant &v : be.models()) {
        QVariantMap m = v.toMap();
        if (m["file"].toString() == model) small = m;
        if (m["isDefault"].toBool()) q36 = m;
    }
    check(!small.isEmpty(), "modelo pequeno listado: " + model);
    check(!small.isEmpty() && !small["usable"].toBool() && small["note"].toString().contains("utilizavel"),
          "modelo pequeno marcado como nao utilizavel (desabilitado) com nota");
    if (!q36.isEmpty())
        check(q36["usable"].toBool(), "Qwen3.6 listado como utilizavel e padrao");
    if (small.isEmpty())
        return 1;

    // ---- sessoes falsas ----
    fprintf(stderr, "-- sessoes falsas\n");
    const QString t1 = "Explique recursao com um exemplo em C";
    fakeSession(dir, "20260101-100000-001", t1, modelo, 300,
                {{"user", t1, {}},
                 {"assistant", "Recursao e quando uma funcao chama a si mesma. Exemplo:\n```c\nint fat(int n) {\n    return n <= 1 ? 1 : n * fat(n - 1);\n}\n```\nO caso base evita a chamada infinita, e `fat(5)` vale **120**.", {}},
                 {"tool", "{\"path\":\"src\"}", "list_dir"}});
    fakeSession(dir, "20260101-090000-002", "Resumir o roteiro de otimizacoes", modelo, 3 * 3600,
                {{"user", "Resuma docs/roteiro-otimizacoes.md", {}}, {"assistant", "O roteiro lista tecnicas ainda nao usadas.", {}}});
    fakeSession(dir, "20251231-100000-003", "Qual a diferenca entre Q4_K_M e Q8_0?", "gemma-4-26B-A4B-it-UD-Q4_K_M.gguf", 26 * 3600,
                {{"user", "Qual a diferenca entre Q4_K_M e Q8_0?", {}}, {"assistant", "Q8_0 usa 8 bits por peso.", {}}});
    fakeSession(dir, "20251229-100000-004", "Escrever um script para renomear arquivos", modelo, 3 * 86400,
                {{"user", "Escrever um script para renomear arquivos", {}}, {"assistant", "Segue um script.", {}}});
    fakeSession(dir, "20251220-100000-005", "Por que o decode e limitado por banda de memoria?", modelo, 12 * 86400,
                {{"user", "Por que o decode e limitado por banda de memoria?", {}}, {"assistant", "Cada token le todos os pesos ativos.", {}}});
    fakeSession(dir, "20251201-100000-006", "Conversa antiga sem muito assunto", modelo, 40 * 86400,
                {{"user", "oi", {}}, {"assistant", "ola", {}}});
    be.refreshSessions();
    QTest::qWait(300);
    check(be.sessions().size() == 6, "6 sessoes listadas pelo backend");
    check(be.sessions().first().toMap()["id"].toString() == "20260101-100000-001", "mais recente primeiro");
    check(c.sessionItems().size() == 6, "6 itens na lateral (QML)");

    // ---- iniciar o modelo pequeno ----
    fprintf(stderr, "-- iniciar agente\n");
    be.start(model);
    QTest::qWait(300);
    check(be.loading(), "estado carregando apos start()");
    check(!c.item("composerInput")->property("enabled").toBool(), "composer desabilitado durante o carregamento");
    check(c.item("composerInput")->property("placeholderText").toString() == "Carregando o modelo...",
          "placeholder 'Carregando o modelo...'");
    check(waitFor([&] { return be.ready(); }, 240000), "agente pronto (READY)");
    check(c.item("composerInput")->property("enabled").toBool(), "composer habilitado quando pronto");
    c.grab("01-inicial.png");

    // ---- tela de boas-vindas (conversa vazia) ----
    fprintf(stderr, "-- tela de boas-vindas\n");
    {
        QQuickItem *em = c.item("emptyState");
        check(em && em->isVisible() && be.messages()->count() == 0, "boas-vindas visivel com a conversa vazia");
        QQuickItem *sm = c.item("samurai");
        check(sm && sm->isVisible() && sm->height() >= 48 && sm->height() <= 96, "emblema K pequeno (48-96 px) e visivel");
        QQuickItem *tt = c.item("welcomeTitle");
        check(tt && tt->property("text").toString() == "Katana", "titulo 'Katana'");
        check(tt && tt->isVisible() && !tt->property("smooth").toBool() && tt->property("status").toInt() == 1,
              "titulo retro em pixel carregado, sem suavizacao");
        QQuickItem *gr = c.item("welcomeGreeting");
        check(gr && gr->property("text").toString().contains("Em que posso ajudar"), "frase de boas-vindas em portugues");
        QList<QQuickItem *> sg;
        findAll(c.root(), "suggestion", sg);
        check(sg.size() == 3, "3 sugestoes clicaveis");
        c.grab(be.themeOverride() == "dark" ? "12-boasvindas-escuro.png" : "12-boasvindas-claro.png");
    }

    // ---- Shift+Enter quebra linha; Enter envia ----
    fprintf(stderr, "-- teclado no composer\n");
    c.click(c.item("composerInput"));
    typeText(win, "a");
    QTest::keyClick(win, Qt::Key_Return, Qt::ShiftModifier);
    typeText(win, "b");
    check(c.inputText() == "a\nb", "Shift+Enter insere quebra de linha");
    check(be.messages()->count() == 0, "Shift+Enter nao enviou");
    {
        // a caixa cresce com o conteudo e para num limite (depois rola)
        QQuickItem *in = c.item("composerInput");
        QQuickItem *fl = c.item("composerFlick");
        const double h1 = fl->height();
        in->setProperty("text", "l1\nl2\nl3\nl4\nl5");
        QTest::qWait(150);
        const double h5 = fl->height();
        check(h5 > h1 + 40, "caixa de texto cresce com varias linhas");
        c.grab("10-caixa-multilinha.png");
        in->setProperty("text", "x\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx\nx");
        QTest::qWait(150);
        check(fl->height() <= 241, "caixa para de crescer no limite");
        c.grab("11-caixa-limite.png");
    }
    c.item("composerInput")->setProperty("text", "");

    // ---- turno com ferramenta ----
    fprintf(stderr, "-- turno com ferramenta (digitado na interface)\n");
    QStringList sent;
    QObject::connect(&be, &Backend::sentToAgent, [&](const QString &l) { sent << l; });
    const QString pergunta = "Quantos arquivos .md existem na pasta docs? Use as ferramentas para listar.";
    c.typeAndEnter(pergunta);
    check(waitFor([&] { return be.messages()->count() >= 1; }, 3000), "mensagem apareceu na lista");
    check(be.messages()->get(0)["kind"] == "user" && be.messages()->get(0)["text"] == pergunta, "balao do usuario com o texto digitado");
    check(c.inputText().isEmpty(), "composer limpo apos enviar");
    check(!c.item("emptyState")->isVisible(), "boas-vindas some apos enviar a primeira mensagem");
    check(waitFor([&] { return be.answering(); }, 3000) || be.ready(), "estado respondendo");
    QTest::qWait(300);
    if (be.answering()) {
        check(c.item("sendButton")->property("tip").toString() == "Parar", "botao vira Parar durante a resposta");
        check(be.thinking() || be.messages()->lastKind() == "assistant", "pontinhos enquanto aguarda o 1o token");
    }
    check(waitFor([&] { return be.ready(); }, 300000), "turno terminou (READY)");
    bool tool = false, listDir = false, xml = false;
    QString resp;
    for (int i = 0; i < be.messages()->count(); ++i) {
        QVariantMap m = be.messages()->get(i);
        if (m["kind"] == "tool") {
            tool = true;
            if (m["name"] == "list_dir") listDir = true;
            if (m["text"].toString().contains('<')) xml = true;
        }
        if (m["kind"] == "assistant") resp += m["text"].toString();
    }
    fprintf(stderr, "  resposta: %s\n", qPrintable(resp.left(300).replace('\n', ' ')));
    check(tool, "apareceu algum cartao de ferramenta");
    check(listDir, "cartao de ferramenta list_dir");
    check(!xml, "argumentos do cartao sem XML");
    check(!resp.contains("<tool_call") && !resp.contains("<|tool_call") && !resp.contains("<tool_response"),
          "texto final sem etiquetas de ferramenta");
    check(!resp.contains("{\""), "texto final sem JSON cru");
    check(!resp.trimmed().isEmpty(), "ha texto de resposta");
    check(!be.speedText().isEmpty(), "estatistica tok/s extraida do stderr: " + be.speedText() + " | " + be.ctxText());
    check(sent.size() == 1 && sent[0] == pergunta, "linha enviada ao agente = texto digitado (sem resumo)");
    c.grab("02-conversa.png");
    const QString sessReal = be.currentSessionId();
    check(QFileInfo::exists(QDir(be.sessionsDir()).filePath(sessReal + ".ksess")),
          "motor gravou a sessao (.ksess) para retomar");

    // ---- marcadores (TOKENS, COMPACT, desconhecidos) ----
    fprintf(stderr, "-- marcadores injetados\n");
    {
        int n0 = be.messages()->count();
        be.injectMarker("WARM 3 4");
        be.injectMarker("QUALQUERCOISA a b c");
        check(be.messages()->count() == n0, "marcadores desconhecidos (WARM etc.) ignorados sem erro");
        be.injectMarker("TOKENS 4321 32768");
        check(be.ctxText() == "contexto 4321 de 32768", "TOKENS atualiza o uso de contexto");
        QQuickItem *meter = c.item("contextMeter");
        check(meter && meter->isVisible(), "indicador de contexto visivel");
        check(meter->property("level").toString() == "ok" && meter->property("pct").toInt() == 13,
              "indicador: 4321/32768 = 13% (normal)");
        c.grab("07-contexto-normal.png");
        be.injectMarker("TOKENS 24000 32768");
        check(meter->property("level").toString() == "warn", "indicador: 73% = aviso");
        c.grab("08-contexto-aviso.png");
        be.injectMarker("TOKENS 30000 32768");
        check(meter->property("level").toString() == "crit", "indicador: 92% = critico");
        check(meter->property("tipText").toString().contains("Livre: 2768"), "dica do indicador mostra o livre");
        c.grab("09-contexto-critico.png");
        be.injectMarker("TOKENS 4321 32768");
        be.injectMarker("COMPACT 30000 8000");
        QVariantMap nm = be.messages()->get(be.messages()->count() - 1);
        check(nm["kind"] == "notice" && nm["text"].toString() == "Contexto compactado: 30000 -> 8000 tokens", "COMPACT vira aviso discreto");
        c.grab("06-aviso.png");
        be.messages()->setAll([&] { QList<MessageModel::Item> l; for (int i = 0; i < be.messages()->count() - 1; ++i) { auto m = be.messages()->get(i); l.append({m["kind"].toString(), m["text"].toString(), m["name"].toString()}); } return l; }());
    }

    // ---- sessao gravada ----
    fprintf(stderr, "-- sessao gravada\n");
    const QString curId = be.currentSessionId();
    check(!curId.isEmpty() && QFileInfo::exists(dir + "/" + curId + ".json"), "arquivo da sessao gravado: " + curId);
    Session s;
    check(Sessoes::carregar(dir, curId, &s), "sessao carregada do disco");
    check(s.titulo == pergunta && s.modelo == model, "titulo = primeira mensagem; modelo gravado");
    bool sTool = false;
    for (const Msg &m : s.mensagens) if (m.papel == "tool" && m.nome == "list_dir") sTool = true;
    check(s.mensagens.size() >= 3 && s.mensagens.first().papel == "user" && sTool, "mensagens (user, tool, assistant) gravadas");
    check(be.sessions().size() == 7 && be.sessions().first().toMap()["atual"].toBool(), "lista tem 7, a atual no topo e destacada");
    QTest::qWait(300);
    check(c.sessionItems().size() == 7, "7 itens na lateral (QML)");

    // ---- dropdown de modelos ----
    fprintf(stderr, "-- dropdown (clique real)\n");
    c.click(c.item("modelCombo"));
    check(waitFor([&] { return c.popupVisible(); }, 2000), "clique abre o dropdown");
    QTest::qWait(300);
    QQuickItem *mlist = c.item("modelList");
    int nEnabled = 0, nDisabled = 0;
    QQuickItem *dSmall = nullptr, *dQ36 = nullptr;
    if (mlist) {
        QQuickItem *ci = mlist->property("contentItem").value<QQuickItem *>();
        for (QQuickItem *d : ci->childItems()) {
            QVariantMap m = d->property("modelData").toMap();
            if (m.isEmpty()) continue;
            (d->isEnabled() ? nEnabled : nDisabled)++;
            if (m["file"] == model) dSmall = d;
            if (m["isDefault"].toBool()) dQ36 = d;
        }
    }
    check(nEnabled >= 1 && nDisabled >= 1, QString("itens habilitados=%1 desabilitados=%2").arg(nEnabled).arg(nDisabled));
    check(dSmall && !dSmall->isEnabled(), "item nao utilizavel esta desabilitado");
    if (dQ36) check(dQ36->isEnabled(), "Qwen3.6 habilitado no dropdown");
    c.grab("03-dropdown.png");
    if (dSmall) c.click(dSmall);  // clique em item desabilitado: nada acontece
    check(be.currentModel() == model && be.ready(), "clicar em item desabilitado nao troca o modelo");
    QTest::keyClick(win, Qt::Key_Escape);
    check(waitFor([&] { return !c.popupVisible(); }, 2000), "Escape fecha o dropdown");

    // ---- abrir sessao antiga pela lateral ----
    fprintf(stderr, "-- abrir sessao antiga (clique real) e resumo pendente\n");
    QQuickItem *it1 = c.sessionItemWithTitle(t1);
    check(it1 != nullptr, "item da sessao antiga na lateral");
    c.click(findItem(it1, "sessionOpen"));
    check(be.currentSessionId() == "20260101-100000-001", "sessao antiga aberta");
    check(be.messages()->count() == 3, "historico mostrado (3 itens: user, assistant, tool)");
    check(be.hasPendingContext(), "resumo pendente criado");
    check(!c.item("emptyState")->isVisible(), "boas-vindas nao aparece em sessao antiga com mensagens");
    check(waitFor([&] { return be.ready(); }, 30000), "agente pronto apos /novo");
    c.grab("04-lateral.png");

    sent.clear();
    c.typeAndEnter("Qual o assunto?");
    check(waitFor([&] { return !sent.isEmpty(); }, 3000), "mensagem enviada");
    if (!sent.isEmpty()) {
        check(sent[0].startsWith("Contexto da conversa anterior: Voce: " + t1), "1a mensagem leva o resumo na mesma linha");
        check(sent[0].endsWith("Nova mensagem: Qual o assunto?") && !sent[0].contains('\n'), "termina com 'Nova mensagem: ...', uma linha so");
    }
    QVariantMap last = be.messages()->get(3);
    check(last["kind"] == "user" && last["text"] == "Qual o assunto?", "a tela mostra so o digitado");
    check(!be.hasPendingContext(), "resumo consumido");
    check(waitFor([&] { return be.ready(); }, 300000), "turno da sessao retomada terminou");
    sent.clear();

    // ---- troca de modelo mantem o resumo pendente ----
    fprintf(stderr, "-- trocar de modelo em sessao antiga mantem o resumo\n");
    QQuickItem *it2 = c.sessionItemWithTitle("Resumir o roteiro de otimizacoes");
    check(it2 != nullptr, "item da 2a sessao antiga");
    c.click(findItem(it2, "sessionOpen"));
    check(be.hasPendingContext() && be.currentSessionId() == "20260101-090000-002", "2a sessao aberta com resumo pendente");
    check(waitFor([&] { return be.ready(); }, 30000), "pronto apos /novo");
    be.start(model);  // reinicia o agente (equivale a escolher outro modelo)
    check(be.loading() && be.hasPendingContext() && be.currentSessionId() == "20260101-090000-002" && be.messages()->count() == 2,
          "apos trocar: resumo, sessao e historico preservados");
    check(waitFor([&] { return be.ready(); }, 240000), "agente reiniciado e pronto");
    c.typeAndEnter("ok");
    check(waitFor([&] { return !sent.isEmpty(); }, 3000) && sent[0].startsWith("Contexto da conversa anterior:"),
          "resumo seguiu na 1a mensagem apos a troca de modelo");
    check(waitFor([&] { return be.ready(); }, 300000), "turno terminou");

    // ---- retomada pelo motor (.ksess: especialistas + contexto compacto) ----
    fprintf(stderr, "-- retomar sessao pelo motor\n");
    be.newChat();
    check(waitFor([&] { return be.ready(); }, 120000), "pronto apos nova conversa");
    sent.clear();
    check(be.openSession(sessReal), "abriu a sessao real");
    check(!be.hasPendingContext(), "sessao com .ksess nao usa o resumo textual");
    check(waitFor([&] { return be.ready(); }, 300000), "retomada terminou");
    check(!be.resumeInfo().isEmpty(), "aviso de retomada: " + be.resumeInfo());
    c.typeAndEnter("Responda so: ok");
    check(waitFor([&] { return !sent.isEmpty(); }, 3000) && sent[0] == "Responda so: ok", "mensagem apos retomar vai sem resumo textual");
    check(waitFor([&] { return be.ready(); }, 300000), "turno apos retomar terminou");

    // ---- parar resposta (SIGINT) ----
    fprintf(stderr, "-- parar resposta (SIGINT)\n");
    be.newChat();
    check(waitFor([&] { return be.ready(); }, 30000), "pronto apos nova conversa");
    check(be.messages()->count() == 0 && be.currentSessionId().isEmpty(), "nova conversa limpa tela e sessao");
    check(c.item("emptyState")->isVisible(), "boas-vindas volta em 'Nova conversa'");
    c.typeAndEnter("Conte de 1 a 500 por extenso, um numero por linha.");
    check(waitFor([&] { return be.answering() && be.messages()->lastKind() == "assistant"; }, 60000), "resposta em andamento");
    QTest::qWait(1500);
    QQuickItem *sb = c.item("sendButton");
    check(sb->property("tip").toString() == "Parar" && sb->isEnabled(), "botao enviar virou Parar");
    c.click(sb);
    check(waitFor([&] { return be.ready(); }, 40000), "agente voltou a ficar pronto apos Parar");
    check(be.running(), "agente continua vivo apos SIGINT");
    qsizetype got = be.messages()->get(be.messages()->count() - 1)["text"].toString().size();
    check(got > 0, QString("texto parcial mantido (%1 caracteres)").arg(got));

    // ---- apagar com confirmacao inline ----
    fprintf(stderr, "-- apagar sessao (cliques reais)\n");
    int before = c.sessionItems().size();
    QQuickItem *victim = c.sessionItemWithTitle("Conversa antiga sem muito assunto");
    check(victim != nullptr, "sessao a apagar visivel");
    c.click(findItem(victim, "sessionDelete"));
    QQuickItem *conf = nullptr;
    for (QQuickItem *s2 : c.sessionItems())
        if (QQuickItem *x = findItem(s2, "confirmDelete")) if (x->isVisible()) conf = x;
    check(conf != nullptr, "confirmacao inline apareceu (sem dialogo)");
    QQuickItem *cancel = findItem(victim, "cancelDelete");
    c.click(cancel);
    QTest::qWait(200);
    check(c.sessionItems().size() == before && QFileInfo::exists(dir + "/20251201-100000-006.json"), "Cancelar mantem a sessao");
    victim = c.sessionItemWithTitle("Conversa antiga sem muito assunto");
    c.click(findItem(victim, "sessionDelete"));
    c.click(findItem(victim, "confirmDelete"));
    QTest::qWait(300);
    check(c.sessionItems().size() == before - 1 && !QFileInfo::exists(dir + "/20251201-100000-006.json"), "Apagar remove da lista e do disco");

    // apagar a sessao atual limpa a tela
    QQuickItem *cur = c.sessionItemWithTitle("Resumir o roteiro de otimizacoes");
    c.click(findItem(cur, "sessionOpen"));
    check(waitFor([&] { return be.ready(); }, 300000) && be.messages()->count() >= 2, "sessao aberta antes de apagar a atual (retomada pelo motor pode recalcular)");
    cur = c.sessionItemWithTitle("Resumir o roteiro de otimizacoes");
    c.click(findItem(cur, "sessionDelete"));
    c.click(findItem(cur, "confirmDelete"));
    check(waitFor([&] { return be.messages()->count() == 0 && be.currentSessionId().isEmpty(); }, 3000), "apagar a sessao atual limpa a conversa");
    check(!QFileInfo::exists(dir + "/20260101-090000-002.json"), "arquivo da sessao atual removido");
    check(waitFor([&] { return be.ready(); }, 30000), "agente pronto de novo");

    // ---- lateral: recolher e gaveta ----
    fprintf(stderr, "-- lateral recolhivel e gaveta\n");
    QQuickItem *side = c.item("sidebar");
    check(side->x() == 0, "lateral aberta em janela larga");
    c.click(c.item("sidebarToggle"));
    check(waitFor([&] { return side->x() <= -side->width() + 1; }, 2000), "botao recolhe a lateral");
    c.click(c.item("sidebarToggle"));
    check(waitFor([&] { return side->x() >= -1; }, 2000), "botao reabre a lateral");
    win->resize(720, 600);
    QTest::qWait(500);
    check(side->x() <= -side->width() + 1, "janela estreita: lateral vira gaveta fechada");
    c.click(c.item("sidebarToggle"));
    check(waitFor([&] { return side->x() >= -1; }, 2000), "gaveta abre pelo botao");
    c.grab("05-estreita.png");
    QTest::keyClick(win, Qt::Key_Escape);
    check(waitFor([&] { return side->x() <= -side->width() + 1; }, 2000), "Escape fecha a gaveta");
    win->resize(1180, 820);

    fprintf(stderr, "== %d verificacoes ok, %d falhas\n", g_ok, g_fail);
    fprintf(stderr, "%s\n", g_fail == 0 ? "PASSOU" : "FALHOU");
    printf("%s\n", g_fail == 0 ? "PASSOU" : "FALHOU");
    return g_fail == 0 ? 0 : 1;
}
