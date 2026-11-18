#include <QFont>
#include "backend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QClipboard>
#include <QRegularExpression>
#include <QTimer>
#include <algorithm>
#ifdef Q_OS_UNIX
#include <signal.h>
#include <unistd.h>
#endif

// ---------- MessageModel ----------

QVariant MessageModel::data(const QModelIndex &i, int role) const
{
    if (!i.isValid() || i.row() < 0 || i.row() >= m_items.size())
        return {};
    const Item &it = m_items[i.row()];
    switch (role) {
    case KindRole: return it.kind;
    case TextRole: return it.text;
    case NameRole: return it.name;
    }
    return {};
}

QHash<int, QByteArray> MessageModel::roleNames() const
{
    return {{KindRole, "kind"}, {TextRole, "text"}, {NameRole, "name"}};
}

QVariantMap MessageModel::get(int i) const
{
    if (i < 0 || i >= m_items.size())
        return {};
    return {{"kind", m_items[i].kind}, {"text", m_items[i].text}, {"name", m_items[i].name}};
}

void MessageModel::append(const QString &kind, const QString &text, const QString &name)
{
    beginInsertRows({}, m_items.size(), m_items.size());
    m_items.append({kind, text, name});
    endInsertRows();
    emit countChanged();
}

void MessageModel::appendToLast(const QString &text)
{
    if (m_items.isEmpty())
        return;
    m_items.last().text += text;
    const QModelIndex i = index(m_items.size() - 1);
    emit dataChanged(i, i, {TextRole});
}

void MessageModel::setAll(const QList<Item> &items)
{
    beginResetModel();
    m_items = items;
    endResetModel();
    emit countChanged();
}

// ---------- TagFilter ----------

static const QList<QByteArray> kTagOpen = {"<tool_call>", "<|tool_call>", "<tool_response>", "<|tool_response>"};
static QByteArray closerFor(const QByteArray &open)
{
    if (open == "<tool_call>") return "</tool_call>";
    if (open == "<|tool_call>") return "<tool_call|>";
    if (open == "<tool_response>") return "</tool_response>";
    return "<tool_response|>";
}

QByteArray TagFilter::feed(const QByteArray &in)
{
    m_buf += in;
    QByteArray out;
    for (;;) {
        if (!m_closer.isEmpty()) {
            int i = m_buf.indexOf(m_closer);
            if (i < 0) {  // guarda so o que pode ser inicio do fechamento
                int keep = m_closer.size() - 1;
                if (m_buf.size() > keep)
                    m_buf = m_buf.right(keep);
                return out;
            }
            m_buf.remove(0, i + m_closer.size());
            m_closer.clear();
            continue;
        }
        QByteArray best;
        int bi = -1;
        for (const QByteArray &t : kTagOpen) {
            int i = m_buf.indexOf(t);
            if (i >= 0 && (bi < 0 || i < bi)) {
                best = t;
                bi = i;
            }
        }
        if (bi >= 0) {
            out += m_buf.left(bi);
            m_buf.remove(0, bi + best.size());
            m_closer = closerFor(best);
            continue;
        }
        // segura sufixo que pode ser comeco de uma etiqueta
        int cut = m_buf.size();
        for (int k = 1; k < 20 && k <= m_buf.size(); ++k) {
            const QByteArray suf = m_buf.right(k);
            for (const QByteArray &t : kTagOpen)
                if (t.startsWith(suf))
                    cut = m_buf.size() - k;
        }
        out += m_buf.left(cut);
        m_buf.remove(0, cut);
        return out;
    }
}

QByteArray TagFilter::flush()
{
    QByteArray s = m_buf;
    m_buf.clear();
    if (!m_closer.isEmpty())
        return {};
    return s;
}

// ---------- Backend ----------

bool Backend::usable(const QString &name, QString *note)
{
    const QString l = name.toLower();
    if (l.contains("qwen3.6") || l.contains("gemma-4") || l.contains("gemma4")) {
        if (note) note->clear();
        return true;
    }
    if (note) *note = QStringLiteral("ainda nao esta utilizavel no agente");
    return false;
}

Backend::Backend(const QString &katana, const QString &modelsDir, const QString &home, QObject *parent)
    : QObject(parent), m_katana(katana), m_modelsDir(modelsDir), m_home(home)
{
    m_sessDir = QDir(home).filePath("sessoes");
    QDir().mkpath(m_sessDir);
    m_ui = new QSettings(QDir(home).filePath("ui.ini"), QSettings::IniFormat, this);
    setFonte(fonte());
    connect(&m_msgs, &MessageModel::rowsInserted, this, &Backend::thinkingChanged);
    connect(&m_msgs, &MessageModel::modelReset, this, &Backend::thinkingChanged);
    connect(&m_msgs, &MessageModel::dataChanged, this, &Backend::thinkingChanged);
    loadModels();
    refreshSessions();
}

Backend::~Backend() { shutdown(); }

void Backend::loadModels()
{
    struct M {
        QVariantMap m;
        bool usable, def;
        QString name;
    };
    QList<M> v;
    const QFileInfoList files = QDir(m_modelsDir).entryInfoList({"*.gguf"}, QDir::Files);
    for (const QFileInfo &fi : files) {
        const QString base = fi.fileName();
        QString note;
        bool ok = usable(base, &note);
        bool def = base.toLower().contains("qwen3.6");
        QString name = base.left(base.size() - 5);
        double gb = fi.size() / 1073741824.0;
        v.append({{{"file", base},
                   {"name", name},
                   {"sizeGb", gb},
                   {"sizeText", QString::number(gb, 'f', 1) + " GB"},
                   {"usable", ok},
                   {"note", note},
                   {"isDefault", def}},
                  ok, def, name});
    }
    std::sort(v.begin(), v.end(), [](const M &a, const M &b) {
        if (a.usable != b.usable) return a.usable;
        if (a.def != b.def) return a.def;
        return a.name < b.name;
    });
    m_models.clear();
    for (const M &m : v)
        m_models.append(m.m);
    emit modelsChanged();
}

void Backend::autoStartDefault()
{
    if (!m_model.isEmpty() || m_proc)
        return;
    for (const QVariant &v : m_models) {
        const QVariantMap m = v.toMap();
        if (m["isDefault"].toBool() && m["usable"].toBool()) {
            start(m["file"].toString());
            return;
        }
    }
}

void Backend::setTexts(const QString &status, const QString &error)
{
    m_status = status;
    m_error = error;
    emit textsChanged();
}

void Backend::clearError()
{
    if (!m_error.isEmpty())
        setTexts(m_status, {});
}

void Backend::setState(bool ready, bool loading, bool answering)
{
    m_ready = ready;
    m_loading = loading;
    m_answering = answering;
    emit stateChanged();
    emit thinkingChanged();
}

// ---- processo ----

void Backend::start(const QString &modelFile)
{
    const QString path = QDir(m_modelsDir).filePath(QFileInfo(modelFile).fileName());
    if (!QFileInfo::exists(path)) {
        setTexts({}, "Modelo nao encontrado: " + modelFile);
        return;
    }
    m_pendingStart = QFileInfo(modelFile).fileName();
    m_hasPendingStart = true;
    if (m_proc)
        detach(m_proc);
    m_proc = nullptr;
    m_model = m_pendingStart;
    // sessao antiga com resumo pendente (ou retomavel pelo motor) sobrevive a troca de modelo; senao recomeca limpo
    m_bound.clear();
    m_resuming = false;
    m_resumeWant.clear();
    if (m_pendingCtx.isEmpty() && m_sess.valida() && QFileInfo::exists(ksessPath(m_sess.id))) {
        m_resumeWant = ksessPath(m_sess.id);  // o motor novo tenta retomar; se recusar (outro modelo), vai o resumo textual
        m_resumeCtx = Sessoes::montarContexto(m_sess.mensagens);
    } else if (m_pendingCtx.isEmpty()) {
        m_sess = Session();
        m_dirty = false;
        m_msgs.clear();
    }
    m_speed.clear();
    m_ctx.clear(); m_ctxUsed = 0; m_ctxTotal = 0;
    setTexts(QStringLiteral("Carregando o modelo (pode levar alguns minutos)..."), {});
    setState(false, true, true);
    emit currentModelChanged();
    emit sessionsChanged();
    maybeLaunch();
}

void Backend::detach(QProcess *p)
{
    p->disconnect(this);
    if (p->state() == QProcess::NotRunning) {
        p->deleteLater();
        return;
    }
    ++m_dying;
    p->write("/sair\n");
    p->closeWriteChannel();
    QTimer::singleShot(3000, p, [p] {
        if (p->state() != QProcess::NotRunning)
            p->kill();
    });
    connect(p, &QProcess::finished, this, [this, p] {
        p->deleteLater();
        --m_dying;
        maybeLaunch();
    });
}

void Backend::maybeLaunch()
{
    if (m_dying > 0 || !m_hasPendingStart)
        return;
    m_hasPendingStart = false;
    launch(m_pendingStart);
}

void Backend::launch(const QString &modelFile)
{
    const QString path = QDir(m_modelsDir).filePath(modelFile);
    auto *p = new QProcess(this);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // configuracao recomendada: variaveis KATANA_* de desempenho/depuracao herdadas do terminal (KATANA_CPU, KATANA_CTX,
    // KATANA_KV16, KATANA_MOE_OLD, KATANA_SWAP, KATANA_VK_*...) sao descartadas para o motor escolher sozinho os padroes
    // mais rapidos. Ficam so as de comportamento do usuario. KATANA_DESKTOP_ENV=1 preserva o ambiente herdado.
    if (env.value("KATANA_DESKTOP_ENV") != "1") {
        static const QStringList manter = {"KATANA_UI", "KATANA_THINK", "KATANA_CONFIRM", "KATANA_WEB", "KATANA_SEARX",
            "KATANA_DL_MAX_MB", "KATANA_GIT_UNSAFE", "KATANA_SYSTEM", "KATANA_DATE", "KATANA_CACHE_DIR", "KATANA_LOG",
            "KATANA_TEMP", "KATANA_MAXTOK", "KATANA_COMPACT", "KATANA_SESS_DIAS", "KATANA_SESS_MB", "KATANA_SESS_RESUMO",
            "KATANA_RETOMAR_TOK", "KATANA_DESKTOP_HOME", "KATANA_DESKTOP_ENV"};
        for (const QString &k : env.keys())
            if (k.startsWith("KATANA_") && !k.startsWith("KATANA_MCP_") && !manter.contains(k))
                env.remove(k);
    }
    env.insert("KATANA_UI", "1");
    env.insert("NO_COLOR", "1");
    if (!env.contains("KATANA_THINK"))
        env.insert("KATANA_THINK", "0");
    p->setProcessEnvironment(env);
#ifdef Q_OS_UNIX
    p->setChildProcessModifier([] { ::setpgid(0, 0); });  // grupo proprio: SIGINT so para o agente
#endif
    p->setProgram(m_katana);
    p->setArguments({"agent", path});
    m_outBuf.clear();
    m_errBuf.clear();
    m_utf8Tail.clear();
    m_filter.reset();
    m_atLineStart = true;
    connect(p, &QProcess::readyReadStandardOutput, this, &Backend::onStdout);
    connect(p, &QProcess::readyReadStandardError, this, &Backend::onStderr);
    connect(p, &QProcess::finished, this, [this, p] { onFinished(p); });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && p == m_proc) {
            m_proc = nullptr;
            setState(false, false, false);
            setTexts({}, "Nao foi possivel iniciar o katana: " + p->errorString());
            emit stateChanged();
        }
    });
    m_proc = p;
    p->start();
}

void Backend::onFinished(QProcess *p)
{
    p->deleteLater();
    if (p != m_proc)
        return;
    m_proc = nullptr;
    setState(false, false, false);
    setTexts({}, "O agente terminou. Escolha um modelo para iniciar de novo.");
}

void Backend::shutdown()
{
    if (m_proc) {
        QProcess *p = m_proc;
        m_proc = nullptr;
        p->disconnect(this);
        p->write("/sair\n");
        p->closeWriteChannel();
        if (!p->waitForFinished(3000)) {
            p->kill();
            p->waitForFinished(3000);
        }
        delete p;
    }
    const auto kids = findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly);
    for (QProcess *k : kids) {
        k->disconnect(this);
        if (k->state() != QProcess::NotRunning) {
            k->kill();
            k->waitForFinished(2000);
        }
        delete k;
    }
}

void Backend::stop()
{
    if (!m_proc)
        return;
#ifdef Q_OS_UNIX
    ::kill(-pid_t(m_proc->processId()), SIGINT);  // Ctrl+C no agente: interrompe a resposta
#else
    start(m_model);  // sem SIGINT: mata e reinicia o agente
#endif
}

void Backend::writeLine(const QString &line)
{
    if (m_proc)
        m_proc->write(line.toUtf8() + "\n");
}

// ---- envio e conversa ----

bool Backend::send(const QString &text)
{
    if (!m_proc || !m_ready || text.trimmed().isEmpty())
        return false;
    QString msg = text;
    if (!m_pendingCtx.isEmpty()) {  // sessao antiga retomada: o agente recebe o resumo antes
        msg = m_pendingCtx + "Nova mensagem: " + text;
        m_pendingCtx.clear();
    }
    const QString line = Sessoes::umaLinha(msg);  // o agente le uma linha por turno
    if (!m_sess.valida())
        m_sess = Sessoes::nova(m_model);
    const QString kp = ksessPath(m_sess.id);
    if (!kp.isEmpty() && kp != m_bound) {  // o motor grava a sessao (calor + conversa compactada) para retomar depois
        writeLine("/sessao " + kp);
        m_bound = kp;
    }
    setTexts({}, {});
    m_speed.clear();
    m_msgs.append("user", text);
    m_sess.mensagens.append({"user", text, {}});
    if (m_sess.titulo.isEmpty())
        m_sess.titulo = Sessoes::truncar(Sessoes::umaLinha(text), 80);
    saveSession();
    setState(false, false, true);
    emit sentToAgent(line);
    writeLine(line);
    refreshSessions();
    return true;
}

bool Backend::newChat()
{
    if (!m_proc) {  // sem agente: so limpa a tela
        m_msgs.clear();
        m_sess = Session();
        m_pendingCtx.clear();
        m_dirty = false;
        refreshSessions();
        return true;
    }
    if (!m_ready)
        return false;
    m_msgs.clear();
    m_sess = Session();
    m_pendingCtx.clear();
    m_bound.clear();
    m_resumeWant.clear();
    m_resumeInfo.clear();
    m_dirty = false;
    m_speed.clear();
    m_ctx.clear(); m_ctxUsed = 0; m_ctxTotal = 0;
    setTexts(QStringLiteral("Preparando a conversa..."), {});
    setState(false, false, true);
    writeLine("/novo");
    refreshSessions();
    return true;
}

bool Backend::openSession(const QString &id)
{
    Session s;
    if (!Sessoes::carregar(m_sessDir, id, &s)) {
        setTexts({}, "Nao foi possivel abrir a conversa.");
        return false;
    }
    const bool agentReady = m_proc && m_ready;
    if (m_proc && !agentReady && !m_loading) {  // respondendo ou preparando
        setTexts({}, "Aguarde o agente terminar.");
        return false;
    }
    m_sess = s;
    m_dirty = false;
    m_pendingCtx = Sessoes::montarContexto(s.mensagens);
    m_resumeInfo.clear();
    const QString kp = ksessPath(s.id);
    const bool canResume = !kp.isEmpty() && QFileInfo::exists(kp);
    if (canResume) {  // o motor retoma: especialistas da sessao + contexto compacto; o resumo textual fica de reserva
        m_resumeCtx = m_pendingCtx;
        m_pendingCtx.clear();
    }
    QList<MessageModel::Item> items;
    for (const Msg &m : s.mensagens)
        items.append({m.papel, m.texto, m.nome});
    m_msgs.setAll(items);
    m_speed.clear();
    m_ctx.clear(); m_ctxUsed = 0; m_ctxTotal = 0;
    if (agentReady && canResume) {
        requestResume(kp);
    } else if (agentReady) {
        setTexts(QStringLiteral("Preparando a conversa..."), {});
        setState(false, false, true);
        m_bound.clear();
        writeLine("/novo");
    } else {
        if (canResume)
            m_resumeWant = kp;  // pede quando o agente terminar de carregar
        setTexts(m_status, {});
    }
    emit stateChanged();
    refreshSessions();
    return true;
}

QString Backend::ksessPath(const QString &id) const
{
    if (!Sessoes::validId(id) || m_sessDir.isEmpty())
        return {};
    return QDir(m_sessDir).filePath(id + ".ksess");
}

void Backend::requestResume(const QString &path)
{
    m_resuming = true;
    m_resumed = false;
    m_bound = path;
    setTexts(QStringLiteral("Retomando a sessao (especialistas e contexto compacto)..."), {});
    setState(false, false, true);
    writeLine("/retomar " + path);
}

bool Backend::deleteSession(const QString &id)
{
    if (!Sessoes::validId(id))
        return false;
    const bool wasCurrent = m_sess.id == id;
    if (!Sessoes::apagar(m_sessDir, id)) {
        setTexts({}, "Nao foi possivel apagar a conversa.");
        return false;
    }
    QFile::remove(ksessPath(id));  // estado do motor (calor e conversa compactada)
    if (m_bound == ksessPath(id))
        m_bound.clear();
    if (wasCurrent) {
        m_sess = Session();
        m_pendingCtx.clear();
        m_dirty = false;
        if (!newChat()) {  // agente ocupado: so limpa a tela
            m_msgs.clear();
        }
    }
    refreshSessions();
    return true;
}

void Backend::refreshSessions()
{
    m_sessions = Sessoes::listar(m_sessDir, m_sess.id);
    emit sessionsChanged();
}

void Backend::saveSession()
{
    if (!m_sess.valida())
        return;
    m_sess.atualizada = Sessoes::agoraIso();
    if (!m_model.isEmpty())
        m_sess.modelo = m_model;
    if (Sessoes::salvar(m_sessDir, m_sess))
        m_dirty = false;
}

void Backend::copyText(const QString &text) { QGuiApplication::clipboard()->setText(text); }

// aparencia: "jetbrains" (padrao) ou "plex"; KATANA_FONTE forca uma
QString Backend::tema() const
{
    QString t = qEnvironmentVariable("KATANA_TEMA");
    if (t.isEmpty()) t = m_ui->value("tema", "escuro").toString();
    return (t == "ide" || t == "claro") ? t : "escuro";
}
void Backend::setTema(const QString &t)
{
    m_ui->setValue("tema", (t == "ide" || t == "claro") ? t : "escuro");
    m_ui->sync();
}

QString Backend::fonte() const
{
    QString f = qEnvironmentVariable("KATANA_FONTE");
    if (f.isEmpty()) f = m_ui->value("fonte", "jetbrains").toString();
    return f == "plex" ? "plex" : "jetbrains";
}
void Backend::setFonte(const QString &f)
{
    m_ui->setValue("fonte", f == "plex" ? "plex" : "jetbrains");
    m_ui->sync();
    QFont fonte = QGuiApplication::font();
    fonte.setFamily(f == "plex" ? "IBM Plex Sans" : "JetBrains Mono");
    QGuiApplication::setFont(fonte);
}

bool Backend::uiFlag(const QString &name) const { return m_ui->value(name, false).toBool(); }
void Backend::setUiFlag(const QString &name, bool v)
{
    m_ui->setValue(name, v);
    m_ui->sync();
}

// ---- traducao da saida ----

void Backend::onStdout()
{
    auto *p = qobject_cast<QProcess *>(sender());
    if (!p || p != m_proc)
        return;
    m_outBuf += p->readAllStandardOutput();
    while (!m_outBuf.isEmpty()) {
        if (m_atLineStart && m_outBuf.at(0) == 0x01) {  // marcador do katana: linha inteira
            int nl = m_outBuf.indexOf('\n');
            if (nl < 0)
                break;
            QByteArray line = m_outBuf.mid(1, nl - 1);
            m_outBuf.remove(0, nl + 1);
            while (line.endsWith('\r'))
                line.chop(1);
            flushText();
            handleMarker(line);
            m_atLineStart = true;
            continue;
        }
        int nl = m_outBuf.indexOf('\n');
        const QByteArray chunk = nl < 0 ? m_outBuf : m_outBuf.left(nl + 1);
        m_outBuf.remove(0, chunk.size());
        emitText(m_filter.feed(chunk));
        m_atLineStart = chunk.endsWith('\n');
    }
}

void Backend::flushText()
{
    emitText(m_filter.flush());
    m_filter.reset();
}

// ponto de extensao: novos marcadores entram aqui; os desconhecidos sao ignorados
void Backend::handleMarker(const QByteArray &line)
{
    const int sp = line.indexOf(' ');
    const QByteArray name = sp < 0 ? line : line.left(sp);
    const QString args = sp < 0 ? QString() : QString::fromUtf8(line.mid(sp + 1));
    if (name == "READY") {
        markReady();
    } else if (name == "TOOL") {
        const int tab = args.indexOf('\t');
        const QString n = tab < 0 ? args : args.left(tab);
        toolCard(n, tab < 0 ? QString() : cleanArgs(args.mid(tab + 1)));
    } else if (name == "TOKENS") {  // TOKENS <pos> <ctx>
        const QStringList a = args.split(' ', Qt::SkipEmptyParts);
        if (a.size() >= 2) {
            setCtx(a[0].toInt(), a[1].toInt());
        }
    } else if (name == "COMPACT") {  // COMPACT <antes> <depois>
        const QStringList a = args.split(' ', Qt::SkipEmptyParts);
        if (a.size() >= 2)
            m_msgs.append("notice", "Contexto compactado: " + a[0] + " -> " + a[1] + " tokens");
    } else if (name == "RESUME") {  // RESUME <msgs> <msgs_total> <tokens> <especialistas> <segundos> <kv|calc>
        const QStringList a = args.split(' ', Qt::SkipEmptyParts);
        if (a.size() >= 6) {
            m_resumed = true;
            m_resumeCtx.clear();
            m_resumeInfo = "Sessao retomada: " + a[2] + " tokens de contexto compacto (" + a[0] + " de " + a[1]
                + " mensagens) em " + a[4] + " s, "
                + (a.size() >= 7 && a[5 + 1] == "kv" ? "cache KV lido do disco" : "contexto recalculado")
                + "; " + a[3] + " especialistas aquecidos";
            m_msgs.append("notice", m_resumeInfo);
        }
    }
    // WARM e outros: ignorados
}

void Backend::setCtx(int used, int total)
{
    m_ctxUsed = used;
    m_ctxTotal = total;
    m_ctx = "contexto " + QString::number(used) + " de " + QString::number(total);
    emit textsChanged();
}

void Backend::markReady()
{
    if (m_resuming) {  // fim do /retomar: sem RESUME o motor nao conseguiu; volta ao resumo textual
        m_resuming = false;
        if (!m_resumed) {
            m_pendingCtx = m_resumeCtx;
            m_bound.clear();
            m_msgs.append("notice", "Nao foi possivel retomar pelo motor; a conversa segue com o resumo do historico");
        }
        m_resumeCtx.clear();
    } else if (!m_resumeWant.isEmpty() && m_proc) {  // agente acabou de carregar com uma sessao aberta
        const QString kp = m_resumeWant;
        m_resumeWant.clear();
        m_resumeCtx = m_pendingCtx;
        m_pendingCtx.clear();
        requestResume(kp);
        return;
    }
    if (m_dirty)
        saveSession();
    setTexts({}, m_error);
    setState(true, false, false);
    refreshSessions();
}

void Backend::toolCard(const QString &name, const QString &args)
{
    m_msgs.append("tool", args, name);
    if (m_sess.valida()) {
        m_sess.mensagens.append({"tool", args, name});
        m_dirty = true;
    }
}

void Backend::emitText(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;
    QByteArray b = m_utf8Tail + bytes;
    m_utf8Tail.clear();
    // separa sufixo UTF-8 incompleto
    for (int k = 1; k <= 3 && k <= b.size(); ++k) {
        const uchar c = uchar(b[b.size() - k]);
        if ((c & 0xC0) == 0x80)
            continue;
        int need = 1;
        if (c >= 0xF0) need = 4;
        else if (c >= 0xE0) need = 3;
        else if (c >= 0xC0) need = 2;
        if (need > k) {
            m_utf8Tail = b.right(k);
            b.chop(k);
        }
        break;
    }
    QString t = QString::fromUtf8(b);
    t.replace("[raciocinio]\n", QString());
    if (!t.isEmpty())
        assistantText(t);
}

void Backend::assistantText(const QString &t)
{
    const bool lastIsAsst = m_msgs.lastKind() == QLatin1String("assistant");
    QString piece = t;
    if (!lastIsAsst) {
        int i = 0;
        while (i < piece.size() && piece[i].isSpace())
            ++i;
        piece = piece.mid(i);
        if (piece.isEmpty())
            return;
        m_msgs.append("assistant", piece);
    } else {
        m_msgs.appendToLast(piece);
    }
    if (m_sess.valida()) {
        if (!m_sess.mensagens.isEmpty() && m_sess.mensagens.last().papel == "assistant")
            m_sess.mensagens.last().texto += piece;
        else
            m_sess.mensagens.append({"assistant", piece, {}});
        m_dirty = true;
    }
}

QString Backend::cleanArgs(const QString &in)
{
    const QString s = in.trimmed();
    // valor texto, ou booleano/numero sem aspas (ex.: edit_file replace_all, que o Gemma 4 manda como true)
    static const QRegularExpression kv(R"re("(\w+)"\s*:\s*(?:"((?:[^"\\]|\\.)*)"|(true|false|-?\d+)(?=\s*[,}]|$)))re");
    if (s.contains('{')) {
        QStringList parts;
        auto it = kv.globalMatch(s);
        while (it.hasNext()) {
            const auto m = it.next();
            const QString raw = m.captured(2).isNull() ? m.captured(3) : m.captured(2);
            QString v;
            for (int i = 0; i < raw.size();) {
                if (raw.mid(i, 2) == "\\\"") { v += '"'; i += 2; }
                else if (raw.mid(i, 3) == "\\\\n") { v += ' '; i += 3; }
                else if (raw.mid(i, 2) == "\\n") { v += ' '; i += 2; }
                else if (raw.mid(i, 2) == "\\\\") { v += '\\'; i += 2; }
                else { v += raw[i]; ++i; }
            }
            parts << m.captured(1) + ": " + v;
        }
        if (!parts.isEmpty())
            return parts.join(" | ");
    }
    static const QRegularExpression tags(R"re(</?[a-z_=|]+[^>]*>)re");
    QString r = s;
    r.replace(tags, " ");
    return r.trimmed();
}

void Backend::onStderr()
{
    auto *p = qobject_cast<QProcess *>(sender());
    if (!p || p != m_proc)
        return;
    m_errBuf += p->readAllStandardError();
    static const QRegularExpression stats(
        R"re(\[entrada: (\d+) tokens, ([\d.]+) tok/s \| resposta: (\d+) tokens, ([\d.]+) tok/s \| contexto (\d+) de (\d+))re");
    int nl;
    while ((nl = m_errBuf.indexOf('\n')) >= 0) {
        const QString l = QString::fromUtf8(m_errBuf.left(nl)).trimmed();
        m_errBuf.remove(0, nl + 1);
        fprintf(stderr, "[katana] %s\n", qPrintable(l));
        const auto m = stats.match(l);
        if (m.hasMatch()) {
            m_speed = m.captured(4) + " tok/s";
            setCtx(m.captured(5).toInt(), m.captured(6).toInt());
        } else if (l.contains("contexto cheio")) {
            setTexts(m_status, "Contexto cheio. Inicie uma conversa nova.");
        } else if (l.contains("interrompido")) {
            setTexts("Interrompido", m_error);
        }
    }
    if (m_errBuf.size() > (1 << 20))
        m_errBuf.clear();
}
