// Selftest de ponta a ponta: sobe o agente com o modelo pequeno, digita e envia pela interface
// (eventos de teclado e mouse reais na janela QML), confere cartoes de ferramenta, sessoes,
// dropdown, apagar com confirmacao, parar com SIGINT, e grava capturas PNG da janela.
#include <QTemporaryDir>
#include "selftest.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QQuickItem>
#include <QRegularExpression>
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
    // o modelo do teste pode ser o de referencia (utilizavel) ou um pequeno forcado pelo teste (nao utilizavel, com nota)
    check(!small.isEmpty() && (small["usable"].toBool() || small["note"].toString().contains("utilizavel")),
          "modelo do teste utilizavel, ou marcado como nao utilizavel com nota");
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
                 {"tool", "{\"command\":\"ls src\"}", "Bash"}});
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
    {   // com o modelo ja em cache o carregamento passa rapido para o preparo: os dois textos valem
        const QString ph = c.item("composerInput")->property("placeholderText").toString();
        check(ph == "Carregando o modelo..." || ph.startsWith("Preparando"), "placeholder de carregamento ou de preparo");
    }
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
        check(fl->height() <= 331, "caixa para de crescer no limite");
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
        check(c.item("sendButton")->property("tip").toString() == "Parar (Esc)", "botao vira Parar durante a resposta");
        check(be.enqueue("mensagem na fila") && be.queue().size() == 1, "mensagem enfileirada durante a resposta");
        be.removeQueued(0);
        check(be.queue().isEmpty(), "item removido da fila");
        check(be.thinking() || be.messages()->lastKind() == "assistant", "pontinhos enquanto aguarda o 1o token");
    }
    check(waitFor([&] { return be.ready(); }, 300000), "turno terminou (READY)");
    bool tool = false, listDir = false, xml = false;
    QString resp;
    for (int i = 0; i < be.messages()->count(); ++i) {
        QVariantMap m = be.messages()->get(i);
        if (m["kind"] == "tool") {
            tool = true;
            if (m["name"] == "Bash") listDir = true;
            if (m["text"].toString().contains('<')) xml = true;
        }
        if (m["kind"] == "assistant") resp += m["text"].toString();
    }
    fprintf(stderr, "  resposta: %s\n", qPrintable(resp.left(300).replace('\n', ' ')));
    check(tool, "apareceu algum cartao de ferramenta");
    check(listDir, "cartao de ferramenta Bash");
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
        be.injectMarker("CHOICE Qual banco de dados devo usar?\tSQLite\tPostgres");
        check(be.choicePending() && be.choiceQuestion() == "Qual banco de dados devo usar?" && be.choiceOptions().size() == 2, "CHOICE: pergunta do agente com 2 opcoes");
        c.grab("10-pergunta.png");
        be.answerChoice("Postgres");
        check(!be.choicePending() && be.choiceOptions().isEmpty(), "responder limpa a pergunta do agente");
        be.injectMarker("TODO [x] Ler o arquivo\\n[ ] Corrigir o erro\\n[ ] Rodar os testes");
        check(be.todoSteps().size() == 3 && be.todoSteps().value(0).startsWith("[x]"), "TODO: lista de tarefas do agente com 3 passos");
        be.injectMarker("MODE plan");
        check(be.planMode(), "MODE plan: modo plano ligado");
        c.grab("11-tarefas-plano.png");
        be.injectMarker("MODE normal");
        {   // hora nas mensagens, andamento de leitura longa e deteccao de travamento
            bool temHora = false;
            for (int i = 0; i < be.messages()->count(); i++) { const QVariantMap mm = be.messages()->get(i); if (mm["kind"] == "user" && mm["time"].toString().size() == 5) temHora = true; }
            check(temHora, "mensagem do usuario leva a hora de envio (HH:mm)");
            be.injectMarker("FEED 2000 512");
            check(be.statusText() == "Lendo 2.000 tokens: 25%", "FEED: andamento da leitura longa: " + be.statusText());
            be.injectMarker("FEED 2000 2000");
            bool st = true;
            check(Backend::activityFor(3, 3, 6.0, false, 0, &st).isEmpty() && !st, "atividade: nada nos primeiros segundos");
            check(Backend::activityFor(70, 2, 6.0, false, 0, &st) == "Trabalhando há 1 min 10 s" && !st, "atividade: tempo do turno");
            check(Backend::activityFor(70, 40, 6.0, false, 0, &st).contains("processando") && !st, "atividade: sem saida mas com processador em uso nao e travamento");
            check(Backend::activityFor(70, 40, 0.0, true, 0, &st).contains("aguardando um comando") && !st, "atividade: comando em curso nao e travamento");
            check(Backend::activityFor(70, 40, 0.0, false, 35, &st).contains("pode ter travado") && st, "atividade: sem saida, processador parado e sem comando = travado");
        }
        be.injectMarker("TODO ");
        check(!be.planMode() && be.todoSteps().isEmpty(), "modo plano desligado e lista limpa");
        // tela de modelos: catalogo e maquina lidos do motor; abre pelo botao; erro de download aparece
        check(waitFor([&] { return be.catalog().size() >= 2 && !be.machine().value("text").toString().isEmpty(); }, 15000),
              "catalogo de modelos e dados da maquina lidos do motor");
        int nrec = 0;
        for (const QVariant &v : be.catalog()) nrec += v.toMap()["recommended"].toBool();
        check(nrec == 1, "catalogo: exatamente um modelo recomendado para esta maquina");
        QQuickItem *store = c.item("modelStore");
        check(store && !store->isVisible() && be.hasUsableModel(), "tela de modelos fechada quando ja ha modelo");
        if (store) {
            store->setProperty("open", true);
            check(waitFor([&] { return store->isVisible(); }, 2000), "tela de modelos abre");
            c.grab("12-modelos.png");
            store->setProperty("open", false);
        }
        {   // download que falha (origem inexistente, pasta temporaria): o erro chega na interface e nada fica pendente
            QTemporaryDir td;
            qputenv("KATANA_PULL_URL", "file:///nao/existe.gguf");
            Backend b2(be.katanaPath(), td.path(), td.path());
            check(!b2.hasUsableModel(), "pasta vazia: nenhum modelo utilizavel (a tela de modelos abre sozinha)");
            b2.pull("gemma-4-26b");
            check(waitFor([&] { return b2.pullId().isEmpty() && !b2.pullError().isEmpty(); }, 30000), "download que falha mostra o erro");
            qunsetenv("KATANA_PULL_URL");
            b2.shutdown();
        }
        if (store) {   // download pelo clique: motor de mentira com o mesmo protocolo de linhas (catalog, pull com PROGRESS e DONE)
            QTemporaryDir td;
            const QString fake = td.filePath("motor.sh"), md = td.filePath("m");
            QDir().mkpath(md);
            QFile f(fake);
            f.open(QIODevice::WriteOnly);
            f.write("#!/bin/sh\n"
                    "d=\"$3\"; [ \"$1\" = pull ] || d=\"$2\"\n"
                    "case \"$1\" in\n"
                    "hw) printf 'ram_gb\\t64\\ngpu\\t-\\nvram_gb\\t0\\ndisk_gb\\t100\\n';;\n"
                    "catalog) st=missing; have=0\n"
                    "  [ -f \"$d/falso.gguf.part\" ] && { st=partial; have=$(wc -c < \"$d/falso.gguf.part\"); }\n"
                    "  [ -f \"$d/falso.gguf\" ] && { st=installed; have=1000; }\n"
                    "  printf 'falso\\tModelo de teste\\tfalso.gguf\\t1000\\t%s\\t%s\\t2\\t1\\tnota\\n' \"$st\" \"$have\"\n"
                    "  printf 'outro\\tOutro\\toutro.gguf\\t1000\\tmissing\\t0\\t2\\t0\\tnota\\n';;\n"
                    "pull) trap 'exit 1' TERM\n"
                    "  n=0; [ -f \"$d/falso.gguf.part\" ] && n=$(wc -c < \"$d/falso.gguf.part\")\n"
                    "  while [ \"$n\" -lt 1000 ]; do\n"
                    "    head -c 100 /dev/zero >> \"$d/falso.gguf.part\"; n=$((n+100))\n"
                    "    printf 'PROGRESS\\t%s\\t1000\\t500\\n' \"$n\"; sleep 0.3\n"
                    "  done\n"
                    "  printf 'VERIFY\\t1000\\n'; mv \"$d/falso.gguf.part\" \"$d/falso.gguf\"; printf 'DONE\\t%s\\n' \"$d/falso.gguf\";;\n"
                    "esac\n");
            f.close();
            QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
            const QString realK = be.katanaPath(), realM = be.modelsDir();
            auto state = [&](const QString &id) {
                for (const QVariant &v : be.catalog()) if (v.toMap()["id"].toString() == id) return v.toMap()["state"].toString();
                return QString();
            };
            be.testRedirect(fake, md);
            store->setProperty("open", true);
            check(waitFor([&] { return state("falso") == "missing" && c.item("storeAction_falso"); }, 5000), "tela de modelos lista o catalogo do motor");
            // o catalogo e relido a cada mudanca e os cartoes sao recriados: buscar o botao de novo a cada uso
            auto act = [&] { QTest::qWait(200); return c.item("storeAction_falso"); };
            auto label = [&] { QQuickItem *a = act(); return a ? a->property("label").toString() : QString(); };
            if (act()) {
                check(label() == "Baixar", "modelo ausente: botao Baixar");
                c.click(act());
                check(waitFor([&] { return be.pullId() == "falso" && be.pullFraction() > 0.05; }, 5000), "clique em Baixar inicia o download e o progresso anda");
                QQuickItem *outro = c.item("storeAction_outro");
                check(label() == "Pausar" && outro && !outro->property("active").toBool(),
                      "durante o download: botao vira Pausar e os outros ficam inativos");
                c.grab("13-baixando.png");
                c.click(act());
                check(waitFor([&] { return be.pullId().isEmpty() && state("falso") == "partial"; }, 5000), "clique em Pausar para o download e guarda a parte baixada");
                check(be.pullError().contains("pausado") && label() == "Continuar", "pausado: aviso e botao Continuar");
                const qint64 parte = QFileInfo(md + "/falso.gguf.part").size();
                check(parte > 0 && parte < 1000, "arquivo parcial no disco");
                c.click(act());
                check(waitFor([&] { return be.pullId() == "falso" && be.pullFraction() * 1000 > parte; }, 5000), "Continuar retoma de onde parou");
                check(waitFor([&] { return be.pullId().isEmpty() && state("falso") == "installed"; }, 15000), "download termina e o modelo aparece instalado");
                check(QFileInfo(md + "/falso.gguf").size() == 1000 && be.pullError().isEmpty() && label() == "Usar",
                      "arquivo completo no disco, sem erro, botao Usar");
            }
            store->setProperty("open", false);
            be.testRedirect(realK, realM);
            check(waitFor([&] { return be.catalog().size() >= 2 && state("falso").isEmpty(); }, 15000), "catalogo real de volta depois do teste");
        }
        {   // atalho (AppImage): a pergunta aparece, o clique grava o atalho no menu e na area de trabalho (pastas temporarias)
            QTemporaryDir td;
            const QString data = td.filePath("share"), desk = td.filePath("Desktop"), app = td.filePath("Meu Katana.AppImage");
            QDir().mkpath(desk);
            check(!be.shortcutOffer() && !c.item("shortcutOffer")->isVisible(), "fora de um AppImage nao oferece atalho");
            be.testShortcut(app, data, desk);
            check(waitFor([&] { return c.item("shortcutOffer")->isVisible() && c.item("shortcutYes"); }, 2000), "pergunta do atalho aparece");
            c.grab("14-atalho.png");
            c.click(c.item("shortcutYes"));
            check(waitFor([&] { return !be.shortcutOffer(); }, 2000) && !c.item("shortcutOffer")->isVisible(), "responder fecha a pergunta");
            QFile m(data + "/applications/katana-desktop.desktop");
            m.open(QIODevice::ReadOnly);
            const QByteArray t = m.readAll();
            check(t.contains("Exec=\"" + app.toUtf8() + "\"") && t.contains("StartupWMClass=katana-desktop"), "atalho do menu aponta para o AppImage");
            check(QFileInfo(data + "/icons/hicolor/256x256/apps/katana-desktop.png").size() > 1000, "icone gravado");
            check(QFileInfo(desk + "/katana-desktop.desktop").isExecutable(), "atalho na area de trabalho, executavel");
        }
        be.messages()->setAll([&] { QList<MessageModel::Item> l; for (int i = 0; i < be.messages()->count() - 1; ++i) { auto m = be.messages()->get(i); l.append({m["kind"].toString(), m["text"].toString(), m["name"].toString()}); } return l; }());
        be.messages()->setAll([&] { QList<MessageModel::Item> l; for (int i = 0; i < be.messages()->count() - 1; ++i) { auto m = be.messages()->get(i); l.append({m["kind"].toString(), m["text"].toString(), m["name"].toString()}); } return l; }());
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
    for (const Msg &m : s.mensagens) if (m.papel == "tool" && m.nome == "Bash") sTool = true;
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
    check(nEnabled >= 1, QString("itens habilitados=%1 desabilitados=%2").arg(nEnabled).arg(nDisabled));
    check(dSmall && dSmall->isEnabled() == dSmall->property("modelData").toMap()["usable"].toBool(),
          "item da lista habilitado so quando o modelo e utilizavel");
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
    check(waitFor([&] { return be.ready(); }, 150000), "agente pronto after /new");
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
    check(waitFor([&] { return be.ready(); }, 150000), "pronto after /new");
    be.start(model);  // reinicia o agente (equivale a escolher outro modelo)
    check(be.loading() && be.hasPendingContext() && be.currentSessionId() == "20260101-090000-002" && be.messages()->count() == 2,
          "apos trocar: resumo, sessao e historico preservados");
    check(waitFor([&] { return be.ready(); }, 240000), "agente reiniciado e pronto");
    c.typeAndEnter("Responda apenas: ok. Nao use ferramentas.");
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
    c.typeAndEnter("Sem usar ferramentas, escreva aqui mesmo um conto longo, de pelo menos 800 palavras, sobre um ferreiro que forja uma katana.");
    check(waitFor([&] { return be.answering() && be.messages()->lastKind() == "assistant"; }, 180000), "resposta em andamento");
    QTest::qWait(200);      // curto: as vezes o modelo encerra a resposta em poucos tokens e nao sobra o que parar
    QQuickItem *sb = c.item("sendButton");
    check(sb->property("tip").toString() == "Parar (Esc)" && sb->isEnabled(), "botao enviar virou Parar");
    c.click(sb);
    check(waitFor([&] { return be.ready(); }, 40000), "agente voltou a ficar pronto apos Parar");
    check(be.running(), "agente continua vivo apos SIGINT");
    qsizetype got = be.messages()->get(be.messages()->count() - 1)["text"].toString().size();
    check(got > 0, QString("texto parcial mantido (%1 caracteres)").arg(got));
    fprintf(stderr, "  resposta parcial: %s\n", qPrintable(be.messages()->get(be.messages()->count() - 1)["text"].toString().left(200)));

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

// ---- teste com motor de mentira: sem modelo, sem GPU; o motor (tools/standin-engine.sh) fala o mesmo protocolo ----
int runStandinTest(Backend &be, QQuickWindow *win, const QString &capDir)
{
    Ctx c{win, &be, capDir};
    be.setTestMode(true);     // abrir arquivo ou pasta so registra o caminho
    fprintf(stderr, "== selftest (motor de mentira): dados %s\n", qPrintable(be.sessionsDir()));
    win->resize(1180, 820);
    win->show();
    QTest::qWait(500);

    fprintf(stderr, "-- partida e especializacoes\n");
    be.autoStartDefault();
    check(waitFor([&] { return be.ready(); }, 90000), "motor de mentira pronto");
    check(waitFor([&] { return be.specsActive().size() == 2 && be.specsSkipped().size() == 1; }, 30000),
          "especializacoes: 2 em uso e 1 pulada, lidas da carga do motor");
    check(be.specsActive().contains("tools.kspec") && be.specsActive().contains("teacher.kspec"), "nomes em uso: " + be.specsActive().join(","));
    check(be.specsSkipped().value(0).startsWith("outro-base.kspec") && be.specsSkipped().value(0).contains("outro modelo base"),
          "pulada com o motivo: " + be.specsSkipped().value(0));
    c.click(c.item("appearanceButton"));
    check(waitFor([&] { QQuickItem *t = c.item("specsActive"); return t && t->isVisible() && t->property("text").toString().contains("tools.kspec"); }, 20000),
          "configuracoes listam as especializacoes em uso");
    check(c.item("specsSkipped") && c.item("specsSkipped")->isVisible() && c.item("specsSkipped")->property("text").toString().contains("outro-base.kspec"),
          "configuracoes listam as puladas");
    c.grab("20-especializacoes.png");
    QDir(be.specsDir()).removeRecursively();
    c.click(c.item("specsFolder"));
    check(QFileInfo(be.specsDir()).isDir() && be.lastOpened() == be.specsDir(), "botao cria a pasta que faltava e a abre: " + be.lastOpened());
    QTest::keyClick(win, Qt::Key_Escape);
    QTest::qWait(200);

    fprintf(stderr, "-- NOTIFY, FILE e FINDING\n");
    c.typeAndEnter("mande os arquivos");
    check(waitFor([&] { return be.ready() && be.messages()->count() >= 7; }, 60000), "pedido terminou");
    QTest::qWait(300);
    int nNotify = 0, nFile = 0, nFind = 0;
    for (int i = 0; i < be.messages()->count(); i++) {
        const QString k = be.messages()->get(i)["kind"].toString();
        nNotify += k == "notify"; nFile += k == "file"; nFind += k == "finding";
    }
    check(nNotify == 1 && nFile == 1 && nFind == 2, QString("mensagens: %1 aviso, %2 arquivo, %3 achados").arg(nNotify).arg(nFile).arg(nFind));
    QQuickItem *nl = c.item("notifyLine");
    check(nl && nl->isVisible(), "aviso aparece como linha destacada");
    QQuickItem *fc = c.item("fileCard");
    check(fc && fc->isVisible() && c.item("fileName")->property("text").toString() == "relatorio final.txt", "cartao do arquivo com o nome");
    QList<QQuickItem *> fi;
    findAll(c.root(), "findingItem", fi);
    check(fi.size() == 2 && c.item("findingsCard"), "achados: lista compacta num so cartao com 2 itens");
    c.grab("21-aviso-arquivo-achados.png");
    c.click(c.item("fileOpen"));
    check(be.lastOpened() == "/tmp/katana-standin/relatorio final.txt", "Abrir: arquivo pelo programa padrao: " + be.lastOpened());
    c.click(c.item("fileFolder"));
    check(be.lastOpened() == "/tmp/katana-standin", "Abrir pasta: " + be.lastOpened());
    if (fi.size() == 2) {
        c.click(fi[0]);
        check(be.lastOpened() == QDir(be.workspace()).filePath("src/engine/a.c"), "achado com linha abre o arquivo (relativo a pasta de trabalho): " + be.lastOpened());
        c.click(fi[1]);
        check(be.lastOpened() == "/tmp/katana-standin/b.c", "achado sem linha abre o arquivo: " + be.lastOpened());
    }
    check(be.messages()->get(be.messages()->count() - 1)["kind"] == "assistant", "pedido com resposta em texto: sem linha de parada");

    fprintf(stderr, "-- agente parou sem responder\n");
    c.typeAndEnter("faca algo mudo");
    check(waitFor([&] { return be.ready() && be.messages()->get(be.messages()->count() - 1)["kind"] == "stopped"; }, 60000),
          "ultimo turno so com ferramenta: linha de parada");
    QTest::qWait(300);
    QQuickItem *sl = c.item("stoppedLine"), *as = c.item("askSummary");
    check(sl && sl->isVisible() && sl->property("text").toString() == "o agente parou sem responder", "linha discreta visivel");
    check(as && as->isVisible(), "botao pedir resumo visivel");
    c.grab("22-parou-sem-responder.png");
    const int antes = be.messages()->count();
    if (as) c.click(as);
    check(waitFor([&] { return be.messages()->get(antes)["kind"] == "user" && be.messages()->get(antes)["text"].toString().contains("Resuma o que foi feito"); }, 30000),
          "pedir resumo manda a mensagem fixa");
    check(waitFor([&] { return be.ready() && be.messages()->get(be.messages()->count() - 1)["text"].toString().startsWith("Resumo:"); }, 60000),
          "o agente responde o resumo");
    QTest::qWait(300);
    check(be.messages()->get(be.messages()->count() - 1)["kind"] == "assistant" && !c.item("askSummary")->isVisible(), "depois do resumo o botao some e nao ha nova linha de parada");
    c.typeAndEnter("pergunta comum");
    check(waitFor([&] { return be.ready() && be.messages()->get(be.messages()->count() - 1)["text"].toString().startsWith("Resposta de teste"); }, 60000)
          && be.messages()->get(be.messages()->count() - 1)["kind"] == "assistant", "resposta normal nao gera linha de parada");

    // ---- plano do pedido: acoes agrupadas por passo, descricao, falha, notas do motor, mudanca do plano, resumo ----
    fprintf(stderr, "-- plano do pedido\n");
    {
        QMetaObject::invokeMethod(win, "applyTheme", Q_ARG(QVariant, QVariant(QStringLiteral("escuro"))));
        c.typeAndEnter("execute o plano");
        // durante a ultima acao (comando de 3 s): o passo 3 esta em andamento
        check(waitFor([&] { return be.progressText().startsWith("passo 3 de 3: Rodar os testes"); }, 30000),
              "progresso durante o pedido: " + be.progressText());
        QTest::qWait(300);
        check(be.progressText().contains("ações") && be.progressText().contains(" · "), "progresso com tempo e numero de acoes: " + be.progressText());
        QQuickItem *sline = c.item("statusLine");
        check(sline && sline->property("text").toString().startsWith("passo 3 de 3"), "linha de estado mostra o passo");
        QQuickItem *pv = c.item("planView");
        check(pv && pv->isVisible(), "o plano aparece na conversa");
        QList<QQuickItem *> rows;
        waitFor([&] { rows.clear(); findAll(c.item("planView"), "actionRow", rows); return !rows.isEmpty(); }, 3000);
        QQuickItem *ws = c.item("workStats");
        check(ws && ws->property("text").toString().contains("ações") && ws->property("text").toString().contains(" · "), "cartao das tarefas mostra tempo e acoes enquanto roda: " + (ws ? ws->property("text").toString() : QString()));
        bool runningDesc = false;
        for (QQuickItem *r : rows)
            if (r->property("running").toBool() && r->property("title").toString() == "Rodar a suite de testes") runningDesc = true;
        check(runningDesc, "acao em andamento: a descricao e a linha principal (passo em andamento aberto)");
        check(rows.size() == 1, QString("so o passo em andamento esta aberto: %1 linha(s) de acao").arg(rows.size()));
        c.grab("30-plano-andamento-escuro.png");
        check(waitFor([&] { return be.ready() && be.messages()->get(be.messages()->count() - 1)["kind"] == "assistant"; }, 30000), "pedido do plano terminou");
        QTest::qWait(500);

        // modelo: grupos, marcas, notas escondidas
        int planId = 0, nTool = 0, nHidden = 0, from = 0;
        for (int i = 0; i < be.messages()->count(); i++)
            if (be.messages()->get(i)["kind"] == "user" && be.messages()->get(i)["text"] == "execute o plano") from = i;
        for (int i = from; i < be.messages()->count(); i++) {
            const QVariantMap m = be.messages()->get(i);
            if (m["kind"] == "plan") planId = m["plan"].toInt();
            if (m["kind"] == "tool") { nTool++; nHidden += m["vkind"] == "hidden"; }
        }
        check(planId > 0, "item do plano na conversa");
        const QVariantMap pd = be.messages()->planData(planId);
        const QVariantList steps = pd["steps"].toList();
        QStringList titles; QList<int> st, cnt;
        for (const QVariant &v : steps) { const QVariantMap m = v.toMap(); titles << m["title"].toString(); st << m["status"].toInt(); cnt << m["count"].toInt(); }
        check(steps.size() == 4 && titles == QStringList({"Ler o projeto", "Corrigir o erro de leitura", "Escrever o teste novo", "Rodar os testes"}),
              "passos: reescrito e novo mantem a ordem; o descartado fica: " + titles.join(" | "));
        check(st == QList<int>({2, 2, 3, 2}), "estados: feito, feito, descartado, feito");
        check(cnt == QList<int>({2, 2, 1, 1}), QString("as acoes ficaram sob o passo em que rodaram (inclusive o descartado): %1,%2,%3,%4").arg(cnt[0]).arg(cnt[1]).arg(cnt[2]).arg(cnt[3]));
        check(pd["done"].toInt() == 3 && pd["total"].toInt() == 3, "3 de 3 passos (o descartado nao conta)");
        check(nTool == 13 && nHidden == 12, QString("13 chamadas, 12 sob o plano (6 de gestao e 6 acoes) e 1 antes dele: %1/%2").arg(nTool).arg(nHidden));
        const QVariantList s2 = steps[1].toMap()["actions"].toList();
        const QVariantMap fail = s2.value(0).toMap();
        const auto &failItem = be.messages()->item(fail["idx"].toInt());
        check(fail["state"].toInt() == 3 && failItem.out.startsWith("error: cc: command not found"), "chamada com erro: marcada como falha");
        check(!failItem.out.contains("[katana:") && failItem.note.startsWith("[katana: 3 calls in a row failed"), "nota do motor fora da saida, guardada a parte");
        check(s2.value(1).toMap()["state"].toInt() == 2 && s2.value(1).toMap()["desc"].toString().isEmpty(), "chamada sem descricao e com sucesso: ok, sem descricao");
        check(steps[0].toMap()["actions"].toList().value(0).toMap()["desc"].toString() == "Listar os arquivos da pasta", "descricao da chamada guardada");
        check(be.progressText().startsWith("3 de 3 passos, 7 ações, "), "resumo do pedido: " + be.progressText());
        check(sline->property("text").toString() == be.progressText(), "resumo na linha de estado");
        check(c.item("workStats")->property("text").toString() == be.progressText(), "resumo tambem no cartao das tarefas");
        // o plano na tela: passos feitos recolhidos, nenhuma acao aberta
        rows.clear(); findAll(c.item("planView"), "actionRow", rows);
        check(rows.isEmpty(), "passos feitos recolhidos em uma linha");
        QList<QQuickItem *> stepItems;
        findAll(c.item("planView"), "planStep", stepItems);
        check(stepItems.size() == 4, QString("4 linhas de passo na tela: %1").arg(stepItems.size()));
        QQuickItem *hdr = c.item("planHeader");
        check(hdr && hdr->property("text").toString().startsWith("Plano · 3 de 3 passos · 7 ações"), "cabecalho do plano: " + (hdr ? hdr->property("text").toString() : QString()));
        c.grab("31-plano-final-escuro.png");
        // abrir o passo 2: a chamada que falhou aparece com marca de falha, e a nota so no detalhe
        if (stepItems.size() == 4) {
            c.click(stepItems[1]);
            rows.clear(); findAll(c.item("planView"), "actionRow", rows);
            check(rows.size() == 2 && rows[0]->property("failed").toBool(), "passo aberto: 2 acoes, a primeira falhou");
            if (rows.size() == 2) {
                c.click(rows[0]);
                check(rows[0]->property("expanded").toBool(), "clicar na acao abre o comando e a saida");
                QTest::qWait(300);
                QQuickItem *nt = nullptr;
                for (QQuickItem *k : rows[0]->childItems()) if (k->objectName() == "actionNote") nt = k;
                QQuickItem *oo = findItem(rows[0], "actionOutput");
                check(nt && nt->isVisible() && nt->property("text").toString().contains("3 calls in a row failed") && !nt->property("text").toString().contains("[katana:"), "notas do motor so no detalhe, como legenda sem os colchetes");
                check(oo && !oo->property("text").toString().contains("[katana:") && oo->property("text").toString().startsWith("error: cc"), "a saida da ferramenta nao traz as notas");
            }
            c.grab("32-plano-passo-aberto-escuro.png");
            c.click(stepItems[1]);
        }
        // tema claro
        QMetaObject::invokeMethod(win, "applyTheme", Q_ARG(QVariant, QVariant(QStringLiteral("claro"))));
        QTest::qWait(300);
        c.grab("33-plano-final-claro.png");
        if (stepItems.size() == 4) {
            c.click(stepItems[1]);
            c.grab("34-plano-passo-aberto-claro.png");
            c.click(stepItems[1]);
        }

        // sessao gravada e restaurada: mesmo agrupamento, sem saida das ferramentas (nao e gravada)
        const QString pid = be.currentSessionId();
        Session ps;
        check(Sessoes::carregar(be.sessionsDir(), pid, &ps), "sessao com plano gravada");
        int mPlan = 0, mDesc = 0, mFail = 0, mStep = 0;
        for (const Msg &m : ps.mensagens) {
            mPlan += m.papel == "plan";
            if (m.papel == "tool") { mDesc += !m.desc.isEmpty(); mFail += m.estado == 3; mStep += m.passo > 0; }
        }
        check(mPlan == 1 && mDesc == 4 && mFail == 1 && mStep == 6, QString("sessao: plano=%1 descricoes=%2 falhas=%3 acoes sob passo=%4").arg(mPlan).arg(mDesc).arg(mFail).arg(mStep));
        be.newChat();
        QTest::qWait(300);
        check(be.openSession(pid), "reabre a sessao com o plano");
        QTest::qWait(600);
        int rPlan = 0, rHidden = 0;
        for (int i = 0; i < be.messages()->count(); i++) {
            const QVariantMap m = be.messages()->get(i);
            if (m["kind"] == "plan") rPlan = m["plan"].toInt();
            rHidden += m["vkind"] == "hidden";
        }
        const QVariantMap rd = be.messages()->planData(rPlan);
        QList<int> rcnt, rst;
        for (const QVariant &v : rd["steps"].toList()) { rcnt << v.toMap()["count"].toInt(); rst << v.toMap()["status"].toInt(); }
        check(rPlan > 0 && rcnt == QList<int>({2, 2, 1, 1}) && rst == QList<int>({2, 2, 3, 2}) && rHidden == 12, "sessao restaurada: mesmos passos, estados e acoes por passo");
        QList<QQuickItem *> rsteps;
        findAll(c.item("planView"), "planStep", rsteps);
        check(rsteps.size() == 4, "sessao restaurada: o plano aparece na conversa");
        QQuickItem *rh = c.item("planHeader");
        check(rh && QRegularExpression("^Plano · 3 de 3 passos · 7 ações · \\d+ s$").match(rh->property("text").toString()).hasMatch(), "sessao restaurada: cabecalho com o tempo do pedido: " + (rh ? rh->property("text").toString() : QString()));
        if (rsteps.size() == 4) {
            c.click(rsteps[1]);
            rows.clear(); findAll(c.item("planView"), "actionRow", rows);
            check(rows.size() == 2 && rows[0]->property("failed").toBool() && rows[0]->property("title").toString() == "Compilar e rodar o programa", "restaurada: marca de falha e descricao");
            c.grab("35-plano-restaurado-claro.png");
            c.click(rsteps[1]);
        }
        QMetaObject::invokeMethod(win, "applyTheme", Q_ARG(QVariant, QVariant(QStringLiteral("escuro"))));
    }

    // ---- Find e Symbols: verbos, resumo dos argumentos, resultado destacado, notas do motor, estado do indice ----
    fprintf(stderr, "-- Find, Symbols e estado do indice\n");
    {
        be.newChat();
        QTest::qWait(300);
        QMetaObject::invokeMethod(win, "applyTheme", Q_ARG(QVariant, QVariant(QStringLiteral("escuro"))));
        be.setIndexForTest(QString(), QString());
        c.typeAndEnter("consulte o indice do projeto");
        // o pedido dura mais de 3 s: a consulta ao `coder status` do motor de mentira acontece durante o trabalho (1a resposta: indexando 40%)
        check(waitFor([&] { return be.indexText() == QStringLiteral("índice: indexando 40%"); }, 20000), "barra: indice indexando 40% enquanto o agente trabalha: " + be.indexText());
        QQuickItem *ix = c.item("indexStatus");
        check(ix && ix->isVisible() && ix->property("text").toString() == be.indexText(), "linha de estado do indice visivel na barra de baixo");
        check(be.indexTip().contains("Arquivos: 31") && be.indexTip().contains("símbolos 40%") && be.indexTip().contains("Rastreador em execução")
              && be.indexTip().contains("Ignorados por regra: 0"), "resumo do indice (arquivos, niveis, ignorados): " + be.indexTip().replace('\n', '|'));
        check(waitFor([&] { return be.ready() && be.messages()->get(be.messages()->count() - 1)["text"].toString().startsWith("Consultei o indice"); }, 60000), "pedido do indice terminou");
        check(waitFor([&] { return be.indexText() == QStringLiteral("índice: pronto"); }, 15000), "ao terminar, uma ultima consulta: indice pronto: " + be.indexText());
        QTest::qWait(300);

        QList<QQuickItem *> rows;
        findAll(c.root(), "actionRow", rows);
        QStringList titles;
        QQuickItem *rFind = nullptr, *rFindNf = nullptr, *rSym = nullptr, *rSymNf = nullptr;
        for (QQuickItem *r : rows) {
            const QString t = r->property("title").toString();
            titles << t;
            if (t == "Procurando no projeto: ct_sb_puts") rFind = r;
            else if (t.startsWith("Procurando no projeto: \"zzqqnotthere\"")) rFindNf = r;
            else if (t == "Consultando os símbolos: callers ct_sb_puts") rSym = r;
            else if (t == "Consultando os símbolos: def nonexistent_fn em tools/glob.c") rSymNf = r;
        }
        check(rFind && rFindNf && rSym && rSymNf, "verbos e resumos de Find e Symbols: " + titles.join(" | "));
        check(titles.contains("Procurando no projeto: \"zzqqnotthere\" (refs)"), "Find mostra a consulta e o modo");
        check(titles.contains("Consultando os símbolos: flow ct_glob"), "Symbols flow: verbo e acao + nome");
        check(titles.contains("Renomeando símbolo: ct_glob -> ct_globber"), "Rename: verbo e resumo de -> para");
        QQuickItem *grp = findItem(c.root(), "activityGroup");
        check(grp != nullptr, "grupo de acoes na conversa");
        if (grp) { grp->setProperty("userSet", true); grp->setProperty("userVal", true); }
        for (QQuickItem *r : {rFind, rFindNf, rSym, rSymNf}) if (r) { r->setProperty("userSet", true); r->setProperty("userVal", true); }
        QTest::qWait(500);
        auto outHtml = [&](QQuickItem *r) { QQuickItem *o = r ? findItem(r, "actionOutput") : nullptr; return o ? o->property("text").toString() : QString(); };
        auto outPlain = [&](QQuickItem *r) { return r ? r->property("outText").toString() : QString(); };
        check(outPlain(rFind).startsWith("Found by name or text") && outPlain(rFind).contains("[confidence high]") && outPlain(rFind).contains("called by: ct_glob")
              && outPlain(rFind).contains("links:"), "Find: resultado inteiro no cartao (confidence, called by, calls, links)");
        const QString hFind = outHtml(rFind), hSym = outHtml(rSym), hNf = outHtml(rFindNf), hSymNf = outHtml(rSymNf);
        check(hFind.contains("<html", Qt::CaseInsensitive) && hFind.contains("[confidence high]") && hFind.contains("called by:") && hFind.contains("font-weight"),
              "Find: texto em formato rico com destaques");
        check(hFind.count("color:#") > hSym.count("color:#") - 100 && hFind.count("color:#") >= 5, QString("Find: trechos coloridos (%1)").arg(hFind.count("color:#")));
        check(hSym.contains("[confidence 96 high; same name, 1 definition; line verified]"), "Symbols: [confidence N level; como foi achado] destacado inteiro");
        check(hNf.contains("NOT FOUND") && hNf.count("color:#") >= 1 && hSymNf.contains("NOT FOUND") && hSymNf.count("color:#") >= 1, "NOT FOUND colorido (Find e Symbols)");
        // o vermelho do tema (Theme.err) aparece no NOT FOUND; o verde-agua do tema (Theme.ty) no confidence high
        check(hNf.contains("#f2928b") && hFind.contains("#6fd1c2"), "cores do tema escuro: erro e confidence high");
        check(!outPlain(rSymNf).contains("[katana:") && outPlain(rSymNf).startsWith("NOT FOUND"), "nota do motor fora da saida de Symbols");
        QQuickItem *nt = findItem(rSymNf, "actionNote");
        check(nt && nt->isVisible() && nt->property("text").toString().startsWith("nota do motor") && nt->property("maximumLineCount").toInt() == 1,
              "nota longa: legenda discreta recolhida em uma linha");
        c.grab("40-find-symbols-escuro.png");
        if (nt) {
            c.click(nt);
            QTest::qWait(200);
            check(nt->property("maximumLineCount").toInt() > 1, "clicar na nota longa mostra tudo");
            c.grab("41-nota-do-motor-aberta-escuro.png");
            c.click(nt);
            check(nt->property("maximumLineCount").toInt() == 1, "clicar de novo recolhe");
        }
        QMetaObject::invokeMethod(win, "applyTheme", Q_ARG(QVariant, QVariant(QStringLiteral("claro"))));
        QTest::qWait(400);
        check(outHtml(rFindNf).contains("#b3261e") && outHtml(rFind).contains("#00796b"), "cores do tema claro: erro e confidence high");
        c.grab("42-find-symbols-claro.png");
        QMetaObject::invokeMethod(win, "applyTheme", Q_ARG(QVariant, QVariant(QStringLiteral("ide"))));
        QTest::qWait(400);
        check(outHtml(rFindNf).contains("#f48771") && outHtml(rFind).contains("#4ec9b0"), "cores do tema IDE: erro e confidence high");
        c.grab("43-find-symbols-ide.png");
        QMetaObject::invokeMethod(win, "applyTheme", Q_ARG(QVariant, QVariant(QStringLiteral("escuro"))));
    }

    fprintf(stderr, "== %d verificacoes ok, %d falhas\n", g_ok, g_fail);
    fprintf(stderr, "%s\n", g_fail == 0 ? "PASSOU" : "FALHOU");
    printf("%s\n", g_fail == 0 ? "PASSOU" : "FALHOU");
    return g_fail == 0 ? 0 : 1;
}
