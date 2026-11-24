#include <memory>
#include <QFont>
#include <QStandardPaths>
#include <QUrl>
#include "backend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QWindow>
#include <QRegularExpression>
#include <QTimer>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QImage>
#include <QImageReader>
#include <QTemporaryFile>
#include <cmath>
#include <QHash>
#include <QLocale>
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
    case OutRole: return it.out;
    case TimeRole: return showTime(it.time);
    case PlanRole: return it.plan;
    case StepRole: return it.step;
    case VKindRole: return it.kind == "tool" && it.step != 0 ? QStringLiteral("hidden") : it.kind;
    }
    return {};
}

QHash<int, QByteArray> MessageModel::roleNames() const
{
    return {{KindRole, "kind"}, {TextRole, "text"}, {NameRole, "name"}, {OutRole, "out"}, {TimeRole, "time"},
            {PlanRole, "plan"}, {StepRole, "step"}, {VKindRole, "vkind"}};
}

QVariantMap MessageModel::get(int i) const
{
    if (i < 0 || i >= m_items.size())
        return {};
    const Item &it = m_items[i];
    return {{"kind", it.kind}, {"text", it.text}, {"name", it.name}, {"out", it.out}, {"time", showTime(it.time)},
            {"desc", it.desc}, {"note", it.note}, {"state", it.state}, {"t0", double(it.t0)}, {"dur", double(it.dur)},
            {"plan", it.plan}, {"step", it.step},
            {"vkind", it.kind == "tool" && it.step != 0 ? QStringLiteral("hidden") : it.kind}};
}

QString MessageModel::showTime(const QString &iso)
{
    if (iso.isEmpty()) return {};
    const QDateTime t = QDateTime::fromString(iso, Qt::ISODate).toLocalTime();
    if (!t.isValid()) return {};
    return t.toString(t.date() == QDate::currentDate() ? "HH:mm" : "dd/MM HH:mm");
}

void MessageModel::append(const QString &kind, const QString &text, const QString &name, const QString &time, const QString &out)
{
    beginInsertRows({}, m_items.size(), m_items.size());
    // mensagens do usuario e do assistente levam a hora (a de agora, se quem chama nao deu outra)
    const QString t = !time.isEmpty() ? time : (kind == "user" || kind == "assistant") ? QDateTime::currentDateTime().toString(Qt::ISODate) : QString();
    m_items.append({kind, text, name, out, t});
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

void MessageModel::setLastToolOut(const QString &out, bool append)
{
    int k = lastRunningTool();
    if (k < 0 && !m_items.isEmpty() && m_items.last().kind == "tool")
        k = m_items.size() - 1;
    if (k < 0)
        return;
    if (append) {
        if (m_items[k].out.size() < (1 << 20))
            m_items[k].out += out;
    } else {
        m_items[k].out = out;
    }
    const QModelIndex i = index(k);
    emit dataChanged(i, i, {OutRole});
    bump();
}

void MessageModel::bump()
{
    if (m_revPending)
        return;
    m_revPending = true;
    QTimer::singleShot(120, this, [this] { m_revPending = false; m_rev++; emit revisionChanged(); });
}
void MessageModel::touch() { bump(); }

int MessageModel::lastToolIndex() const
{
    for (int i = m_items.size() - 1; i >= 0; i--)
        if (m_items[i].kind == "tool") return i;
    return -1;
}

int MessageModel::lastRunningTool() const
{
    for (int i = m_items.size() - 1; i >= 0; i--)
        if (m_items[i].kind == "tool" && m_items[i].state == 1) return i;
    return -1;
}

int MessageModel::appendTool(const QString &name, const QString &args, const QString &desc, int plan, int step)
{
    beginInsertRows({}, m_items.size(), m_items.size());
    Item it{"tool", args, name, {}, {}};
    it.desc = desc;
    it.state = 1;
    it.t0 = QDateTime::currentMSecsSinceEpoch();
    it.plan = plan;
    it.step = step;
    m_items.append(it);
    endInsertRows();
    emit countChanged();
    bump();
    return m_items.size() - 1;
}

void MessageModel::setToolStep(int idx, int step)
{
    if (idx < 0 || idx >= m_items.size()) return;
    m_items[idx].step = step;
    const QModelIndex i = index(idx);
    emit dataChanged(i, i, {StepRole, VKindRole});
    bump();
}

// notas do motor dentro do resultado ("[katana: ...]", "[plan: ...]"): sao para o modelo, nao para o usuario. Podem estar no inicio,
// no meio ou no fim; uma nota comeca numa linha que abre com o marcador e vai ate a linha que fecha com "]" (ate 12 linhas)
void MessageModel::splitNotes(const QString &raw, QString *clean, QString *note)
{
    *clean = raw;
    note->clear();
    if (!raw.contains(QLatin1String("[katana:")) && !raw.contains(QLatin1String("[plan:")))
        return;
    const QStringList ln = raw.split('\n');
    QStringList keep, notes;
    for (int i = 0; i < ln.size(); i++) {
        const QString t = ln[i].trimmed();
        if (!t.startsWith(QLatin1String("[katana:")) && !t.startsWith(QLatin1String("[plan:"))) { keep << ln[i]; continue; }
        int end = -1;
        for (int j = i; j < ln.size() && j < i + 12; j++)
            if (ln[j].trimmed().endsWith(']')) { end = j; break; }
        if (end < 0) end = i;
        QStringList part;
        for (int j = i; j <= end; j++) part << ln[j].trimmed();
        notes << part.join('\n');
        i = end;
    }
    if (notes.isEmpty())
        return;
    *clean = keep.join('\n').trimmed();
    *note = notes.join('\n');
}

// mesma regra do motor (rp_failed): erro da ferramenta, ou comando com codigo de saida diferente de zero
bool MessageModel::resultFailed(const QString &clean)
{
    if (clean.startsWith(QLatin1String("error:")))
        return true;
    static const QRegularExpression re(R"re((?:Exit code |\[exit code )([1-9]))re");
    return re.match(clean).hasMatch();
}

void MessageModel::finishTool(int idx, const QString &result, qint64 nowMs)
{
    if (idx < 0 || idx >= m_items.size()) return;
    Item &it = m_items[idx];
    QString clean, note;
    splitNotes(result, &clean, &note);
    it.out = clean.isEmpty() && note.isEmpty() ? QStringLiteral("(sem saida)") : clean;
    it.note = note;
    it.state = resultFailed(clean) ? 3 : 2;
    it.dur = qMax<qint64>(0, nowMs - it.t0);
    const QModelIndex i = index(idx);
    emit dataChanged(i, i, {OutRole});
    bump();
}

void MessageModel::finishRunning(qint64 nowMs, bool failed)
{
    for (int k = 0; k < m_items.size(); k++) {
        Item &it = m_items[k];
        if (it.kind == "tool" && it.state == 1) {
            it.state = failed ? 3 : 2;
            it.dur = qMax<qint64>(0, nowMs - it.t0);
            const QModelIndex i = index(k);
            emit dataChanged(i, i, {OutRole});
        }
    }
    bump();
}

// ---- planos ----

PlanInfo *MessageModel::plan(int id)
{
    for (PlanInfo &p : m_plans) if (p.id == id) return &p;
    return nullptr;
}

int MessageModel::maxPlanId() const
{
    int m = 0;
    for (const PlanInfo &p : m_plans) m = qMax(m, p.id);
    return m;
}

static void parseTodo(const QStringList &todo, QList<QPair<bool, QString>> *out)
{
    for (const QString &l : todo) {
        const bool done = l.startsWith("[x]") || l.startsWith("[X]");
        QString t = l;
        if (t.startsWith("[x]") || t.startsWith("[X]") || t.startsWith("[ ]")) t = t.mid(3);
        t = t.simplified();
        if (!t.isEmpty()) out->append({done, t});
    }
}

int MessageModel::addPlan(const QStringList &todo, qint64 t0)
{
    PlanInfo p;
    p.id = maxPlanId() + 1;
    p.t0 = t0;
    m_plans.append(p);
    updatePlan(p.id, todo);
    return p.id;
}

// reconcilia a lista nova com os passos lembrados: mesmo texto = mesmo passo; mesma posicao com outro texto = passo reescrito;
// o que sumiu da lista fica como descartado (as acoes ja rodadas continuam sob ele); o que e novo ganha id novo
void MessageModel::updatePlan(int id, const QStringList &todo)
{
    PlanInfo *p = plan(id);
    if (!p) return;
    QList<QPair<bool, QString>> n;
    parseTodo(todo, &n);
    const QList<PlanStep> old = p->steps;
    QList<int> map(n.size(), -1);       // passo novo -> posicao no antigo
    QList<bool> used(old.size(), false);
    auto same = [](const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) == 0; };
    for (int ni = 0; ni < n.size(); ni++)          // 1. o mesmo texto (os ainda ativos primeiro)
        for (int pass = 0; pass < 2 && map[ni] < 0; pass++)
            for (int oi = 0; oi < old.size(); oi++)
                if (!used[oi] && (pass == 1 || old[oi].status != 3) && same(old[oi].title, n[ni].second)) { map[ni] = oi; used[oi] = true; break; }
    for (int ni = 0; ni < n.size(); ni++)          // 2. a mesma posicao com outro texto: o passo foi reescrito
        if (map[ni] < 0 && ni < old.size() && !used[ni] && old[ni].status != 3) { map[ni] = ni; used[ni] = true; }
    QList<PlanStep> res;
    QList<bool> emitted(n.size(), false);
    QList<int> resOf(n.size(), -1);
    auto emitNew = [&](int ni) {
        if (emitted[ni]) return;
        emitted[ni] = true;
        PlanStep s;
        if (map[ni] >= 0) s = old[map[ni]];
        else s.id = p->nextStep++;
        s.title = n[ni].second;
        resOf[ni] = res.size();
        res.append(s);
    };
    int nextEmit = 0;
    for (int oi = 0; oi < old.size(); oi++) {
        const int ni = map.indexOf(oi);
        if (ni >= 0) {
            for (int k = nextEmit; k <= ni; k++) emitNew(k);
            nextEmit = qMax(nextEmit, ni + 1);
        } else {
            PlanStep s = old[oi];
            s.status = 3;
            res.append(s);
        }
    }
    for (int k = 0; k < n.size(); k++) emitNew(k);
    bool doing = false;                            // o primeiro passo nao feito esta em andamento
    for (int k = 0; k < n.size(); k++) {
        PlanStep &s = res[resOf[k]];
        if (n[k].first) s.status = 2;
        else if (!doing) { s.status = 1; doing = true; }
        else s.status = 0;
    }
    p->steps = res;
    bump();
}

void MessageModel::endPlan(int id, qint64 t1)
{
    if (PlanInfo *p = plan(id)) { p->t1 = t1; bump(); }
}

void MessageModel::insertPlanItem(int id)
{
    beginInsertRows({}, m_items.size(), m_items.size());
    Item it{"plan", {}, {}, {}, {}};
    it.plan = id;
    m_items.append(it);
    endInsertRows();
    emit countChanged();
    bump();
}

int MessageModel::currentStep(int id) const
{
    for (const PlanInfo &p : m_plans) {
        if (p.id != id) continue;
        int last = 0;
        for (const PlanStep &s : p.steps) {
            if (s.status == 1) return s.id;
            if (s.status == 2) last = s.id;
        }
        return last;
    }
    return 0;
}

QVariantMap MessageModel::planData(int id) const
{
    QVariantMap r;
    for (const PlanInfo &p : m_plans) {
        if (p.id != id) continue;
        QHash<int, int> pos;
        QVariantList steps;
        int done = 0, total = 0;
        for (const PlanStep &s : p.steps) {
            pos[s.id] = steps.size();
            steps.append(QVariantMap{{"id", s.id}, {"title", s.title}, {"status", s.status}, {"actions", QVariantList()},
                                     {"count", 0}, {"ms", 0.0}, {"failed", 0}, {"t0", 0.0}});
            if (s.status != 3) { total++; if (s.status == 2) done++; }
        }
        for (int i = 0; i < m_items.size(); i++) {
            const Item &it = m_items[i];
            if (it.kind != "tool" || it.plan != id || it.step <= 0 || !pos.contains(it.step)) continue;
            QVariantMap st = steps[pos[it.step]].toMap();
            QVariantList acts = st["actions"].toList();
            acts.append(QVariantMap{{"idx", i}, {"name", it.name}, {"args", it.text}, {"desc", it.desc}, {"note", it.note},
                                    {"state", it.state}, {"t0", double(it.t0)}, {"dur", double(it.dur)}, {"hasOut", !it.out.isEmpty()}});
            st["actions"] = acts;
            st["count"] = st["count"].toInt() + 1;
            if (it.dur > 0) st["ms"] = st["ms"].toDouble() + it.dur;
            if (it.state == 3) st["failed"] = st["failed"].toInt() + 1;
            if (it.t0 && st["t0"].toDouble() == 0.0) st["t0"] = double(it.t0);
            steps[pos[it.step]] = st;
        }
        r = {{"id", id}, {"steps", steps}, {"done", done}, {"total", total}, {"t0", double(p.t0)}, {"t1", double(p.t1)}, {"acts", p.acts}};
        break;
    }
    return r;
}

QString MessageModel::planJson(int id) const
{
    for (const PlanInfo &p : m_plans) {
        if (p.id != id) continue;
        QJsonArray st;
        for (const PlanStep &s : p.steps) st.append(QJsonObject{{"id", s.id}, {"t", s.title}, {"s", s.status}});
        return QString::fromUtf8(QJsonDocument(QJsonObject{{"id", p.id}, {"next", p.nextStep}, {"t0", double(p.t0)}, {"t1", double(p.t1)},
                                                           {"acts", p.acts}, {"steps", st}}).toJson(QJsonDocument::Compact));
    }
    return {};
}

void MessageModel::restorePlan(const QString &json)
{
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    if (o.isEmpty()) return;
    PlanInfo p;
    p.id = o["id"].toInt();
    p.nextStep = o["next"].toInt(1);
    p.t0 = qint64(o["t0"].toDouble());
    p.t1 = qint64(o["t1"].toDouble());
    if (p.t1 == 0) p.t1 = -1;          // a sessao foi gravada no meio do pedido: o fim e desconhecido
    p.acts = o["acts"].toInt();
    for (const QJsonValue &v : o["steps"].toArray()) {
        const QJsonObject s = v.toObject();
        PlanStep st;
        st.id = s["id"].toInt();
        st.title = s["t"].toString();
        st.status = s["s"].toInt();
        p.steps.append(st);
    }
    m_plans.append(p);
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
    // modelos com formato que o agente entende (ChatML, Gemma 4, gpt-oss) e tamanho para usar ferramentas; os pequenos/antigos ficam desabilitados
    if (l.contains("qwen3") || l.contains("gemma-4") || l.contains("gemma4") || l.contains("gpt-oss") || l.contains("ravenx")
        || l.contains("qwen-agentworld")) {     // mesma arquitetura e formato (ChatML) do Qwen3.6
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
    {   QProcess v; v.start(m_katana, {"version"});                       // versao do motor em uso (caminho e compilacao)
        if (v.waitForFinished(3000)) m_engineVersion = QString::fromUtf8(v.readAllStandardOutput()).trimmed();
        if (m_engineVersion.isEmpty()) m_engineVersion = "versao do motor desconhecida";
        {   QProcess c; c.start(m_katana, {"version", "--changes"});
            if (c.waitForFinished(3000)) m_engineChanges = QString::fromUtf8(c.readAllStandardOutput()).trimmed(); }
        m_engineVersion += " · " + QFileInfo(m_katana).absoluteFilePath(); }
    QDir().mkpath(m_sessDir);
    m_ui = new QSettings(QDir(home).filePath("ui.ini"), QSettings::IniFormat, this);
    setFonte(fonte());
    connect(&m_msgs, &MessageModel::rowsInserted, this, &Backend::thinkingChanged);
    connect(&m_msgs, &MessageModel::modelReset, this, &Backend::thinkingChanged);
    connect(&m_msgs, &MessageModel::dataChanged, this, &Backend::thinkingChanged);
    connect(&m_msgs, &MessageModel::modelReset, this, [this] { if (!m_answering) { m_progress.clear(); m_stats.clear(); emit progressChanged(); } });
    connect(&m_msgs, &MessageModel::revisionChanged, this, [this] { updateProgress(); });
    loadModels();
    refreshCatalog();
    refreshSessions();
    {   // atividade do agente enquanto trabalha (tempo, processador, travamento)
        auto *t = new QTimer(this);
        connect(t, &QTimer::timeout, this, &Backend::actTick);
        t->start(1000);
    }
    {   // estado do indice de codigo: so enquanto o agente trabalha (e uma vez ao terminar)
        auto *t = new QTimer(this);
        connect(t, &QTimer::timeout, this, &Backend::indexPoll);
        t->start(3000);
        connect(this, &Backend::workspaceChanged, this, [this] { m_idxText.clear(); m_idxTip.clear(); emit indexChanged(); });
    }
    {   // uso da maquina na barra de baixo
        auto *t = new QTimer(this);
        connect(t, &QTimer::timeout, this, &Backend::sysTick);
        t->start(2000);
        sysTick();
    }
    // rodando de um AppImage: oferece o atalho uma vez; se ja foi aceito e o arquivo mudou de lugar, conserta o caminho
    m_scApp = qEnvironmentVariable("APPIMAGE");
    if (!m_scApp.isEmpty()) {
        m_scData = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        m_scDesk = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
        const QString st = m_ui->value("atalho").toString();
        if (st.isEmpty()) m_scOffer = true;
        else if (st == "sim" && m_ui->value("atalhoApp").toString() != m_scApp) {
            installShortcut(m_scApp, m_scData, m_scDesk, false);
            m_ui->setValue("atalhoApp", m_scApp);
            m_ui->sync();
        }
    }
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

bool Backend::hasUsableModel() const
{
    for (const QVariant &v : m_models)
        if (v.toMap()["usable"].toBool()) return true;
    return false;
}

// texto de cada modelo do catalogo na interface (o motor manda uma nota sem acentos, usada so se o id for novo)
static QString catalogNote(const QString &id, const QString &fallback)
{
    static const QHash<QString, QString> t = {
        {"gemma-4-26b", "Pede 24 GB de memória ou mais; usa a placa de vídeo que houver."},
        {"qwen3.6-35b", "Pede 32 GB de memória ou mais; o melhor com ferramentas e programação."},
    };
    return t.value(id, fallback);
}

static QString gbText(double bytes)
{
    return QLocale(QLocale::Portuguese).toString(bytes / 1073741824.0, 'f', 1) + " GB";
}

void Backend::refreshCatalog()
{
    QDir().mkpath(m_modelsDir);
    auto *h = new QProcess(this);
    connect(h, &QProcess::finished, this, [this, h] {
        QVariantMap m;
        for (const QByteArray &l : h->readAllStandardOutput().split('\n')) {
            const int t = l.indexOf('\t');
            if (t > 0) m[QString::fromUtf8(l.left(t))] = QString::fromUtf8(l.mid(t + 1)).trimmed();
        }
        h->deleteLater();
        if (m.isEmpty()) return;
        const QString gpu = m["gpu"].toString();
        QString txt = QString::number(qRound(m["ram_gb"].toDouble())) + " GB de memória";
        if (gpu != "-" && !gpu.isEmpty()) txt += " · " + gpu + " (" + QString::number(qRound(m["vram_gb"].toDouble())) + " GB)";
        else txt += " · sem placa de vídeo compatível";
        txt += " · " + QString::number(qRound(m["disk_gb"].toDouble())) + " GB livres em disco";
        m["text"] = txt;
        m_machine = m;
        emit catalogChanged();
    });
    h->start(m_katana, {"hw", m_modelsDir});
    auto *c = new QProcess(this);
    connect(c, &QProcess::finished, this, [this, c] {
        QVariantList v;
        for (const QByteArray &l : c->readAllStandardOutput().split('\n')) {
            const QList<QByteArray> f = l.split('\t');
            if (f.size() < 9) continue;
            const QString id = QString::fromUtf8(f[0]);
            const double total = f[3].toDouble(), have = f[5].toDouble();
            v.append(QVariantMap{{"id", id}, {"name", QString::fromUtf8(f[1])}, {"file", QString::fromUtf8(f[2])},
                                 {"sizeText", gbText(total)}, {"state", QString::fromUtf8(f[4])},
                                 {"haveFraction", total > 0 ? have / total : 0.0}, {"fit", f[6].toInt()},
                                 {"recommended", f[7].toInt() == 1}, {"note", catalogNote(id, QString::fromUtf8(f[8]).trimmed())}});
        }
        c->deleteLater();
        m_catalog = v;
        emit catalogChanged();
    });
    c->start(m_katana, {"catalog", m_modelsDir});
}

void Backend::pull(const QString &id)
{
    if (m_pull || id.isEmpty()) return;
    m_pullId = id;
    m_pullFrac = 0;
    m_pullText = "Iniciando o download...";
    m_pullError.clear();
    m_pullBuf.clear();
    emit pullChanged();
    QDir().mkpath(m_modelsDir);
    m_pull = new QProcess(this);
    QProcess *p = m_pull;
    connect(p, &QProcess::readyReadStandardOutput, this, [this, p] {
        m_pullBuf += p->readAllStandardOutput();
        int nl;
        while ((nl = m_pullBuf.indexOf('\n')) >= 0) {
            const QByteArray line = m_pullBuf.left(nl);
            m_pullBuf.remove(0, nl + 1);
            onPullLine(line);
        }
    });
    connect(p, &QProcess::finished, this, [this, p] {
        p->deleteLater();
        if (m_pull != p) return;
        m_pull = nullptr;
        if (!m_pullId.isEmpty()) {          // terminou sem DONE nem ERROR: cancelado ou morto
            if (m_pullError.isEmpty()) m_pullError = "Download interrompido. O que já foi baixado fica guardado: é só continuar.";
            m_pullId.clear();
            emit pullChanged();
        }
        refreshCatalog();
    });
    p->start(m_katana, {"pull", id, m_modelsDir});
}

void Backend::onPullLine(const QByteArray &line)
{
    const QList<QByteArray> f = line.split('\t');
    if (f.isEmpty()) return;
    if (f[0] == "PROGRESS" && f.size() >= 4) {
        const double have = f[1].toDouble(), total = f[2].toDouble(), rate = f[3].toDouble();
        m_pullFrac = total > 0 ? have / total : 0;
        QString t = gbText(have) + " de " + gbText(total);
        if (rate > 0) {
            t += " · " + QLocale(QLocale::Portuguese).toString(rate / 1048576.0, 'f', 1) + " MB/s";
            const int left = int((total - have) / rate);
            t += left >= 120 ? " · faltam " + QString::number((left + 59) / 60) + " min" : " · faltam " + QString::number(left) + " s";
        }
        m_pullText = t;
        emit pullChanged();
    } else if (f[0] == "VERIFY") {
        m_pullFrac = 1;
        m_pullText = "Conferindo o arquivo baixado...";
        emit pullChanged();
    } else if (f[0] == "ERROR") {
        m_pullError = f.size() > 1 ? QString::fromUtf8(f[1]) : QStringLiteral("o download falhou");
        m_pullId.clear();
        emit pullChanged();
    } else if (f[0] == "DONE" && f.size() > 1) {
        const QString file = QFileInfo(QString::fromUtf8(f[1]).trimmed()).fileName();
        m_pullId.clear();
        m_pullText.clear();
        emit pullChanged();
        loadModels();
        if (!m_proc && m_model.isEmpty()) start(file);     // primeiro modelo da maquina: ja abre com ele
    }
}

// grava katana-desktop.desktop (mesmo nome do app, para a dock casar com a janela) e o icone na pasta do usuario
bool Backend::installShortcut(const QString &app, const QString &dataDir, const QString &desktopDir, bool dock)
{
    if (app.isEmpty() || dataDir.isEmpty()) return false;
    const QString iconDir = dataDir + "/icons/hicolor/256x256/apps", appsDir = dataDir + "/applications";
    QDir().mkpath(iconDir);
    QDir().mkpath(appsDir);
    const QString icon = iconDir + "/katana-desktop.png";
    QFile::remove(icon);
    QFile::copy(":/icons/katana-256.png", icon);
    QFile::setPermissions(icon, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther);
    QString q = app;
    q.replace("\\", "\\\\").replace("\"", "\\\"").replace("$", "\\$").replace("`", "\\`");
    const QByteArray txt = ("[Desktop Entry]\nType=Application\nName=Katana\nGenericName=Agente de IA local\n"
                            "Comment=Agente de IA que roda no seu computador\nExec=\"" + q + "\"\nIcon=" + icon
                            + "\nTerminal=false\nCategories=Utility;\nStartupWMClass=katana-desktop\n").toUtf8();
    auto put = [&](const QString &path) {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        f.write(txt);
        f.close();
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup
                                        | QFile::ReadOther | QFile::ExeOther);
        return true;
    };
    if (!put(appsDir + "/katana-desktop.desktop")) return false;
    if (!desktopDir.isEmpty() && QDir(desktopDir).exists()) {
        const QString p = desktopDir + "/katana-desktop.desktop";
        // o GNOME so abre atalho da area de trabalho marcado como confiavel
        if (put(p)) QProcess::startDetached("gio", {"set", p, "metadata::trusted", "true"});
    }
    if (dock && qEnvironmentVariable("XDG_CURRENT_DESKTOP").contains("GNOME", Qt::CaseInsensitive)) {
        QProcess g;
        g.start("gsettings", {"get", "org.gnome.shell", "favorite-apps"});
        if (g.waitForFinished(3000) && g.exitCode() == 0) {
            QString l = QString::fromUtf8(g.readAllStandardOutput()).trimmed();
            const int fim = l.lastIndexOf(']');
            if (fim > 0 && !l.contains("'katana-desktop.desktop'")) {
                const bool vazio = l.indexOf('[') >= 0 && l.mid(l.indexOf('[') + 1, fim - l.indexOf('[') - 1).trimmed().isEmpty();
                l = l.mid(l.indexOf('['), fim - l.indexOf('[')) + (vazio ? "" : ", ") + "'katana-desktop.desktop']";
                QProcess::startDetached("gsettings", {"set", "org.gnome.shell", "favorite-apps", l});
            }
        }
    }
    return true;
}

void Backend::answerShortcut(bool yes)
{
    if (!m_scOffer) return;
    if (yes) installShortcut(m_scApp, m_scData, m_scDesk, !m_scTest);
    if (!m_scTest) {
        m_ui->setValue("atalho", yes ? "sim" : "nao");
        m_ui->setValue("atalhoApp", m_scApp);
        m_ui->sync();
    }
    m_scOffer = false;
    emit shortcutChanged();
}

static QString secsText(int s)
{
    return s < 60 ? QString::number(s) + " s" : QString::number(s / 60) + " min " + QString::number(s % 60).rightJustified(2, '0') + " s";
}

// quietSecs: segundos sem nenhuma saida do motor; cpuCores: nucleos em uso pelo agente e seus comandos;
// children: ha comando em curso; idleSecs: segundos seguidos com o processador parado e sem comando
QString Backend::activityFor(int turnSecs, int quietSecs, double cpuCores, bool children, int idleSecs, bool *stalled)
{
    if (stalled) *stalled = false;
    if (turnSecs < 5) return {};
    const QString base = "Trabalhando há " + secsText(turnSecs);
    if (quietSecs < 15) return base;
    if (cpuCores >= 0.5) return base + " · processando (sem saída nova há " + secsText(quietSecs) + ", processador em uso)";
    if (children) return base + " · aguardando um comando terminar (" + secsText(quietSecs) + ")";
    if (idleSecs >= 30) {
        if (stalled) *stalled = true;
        return "Sem atividade há " + secsText(quietSecs) + ": o agente pode ter travado. Esc interrompe a resposta.";
    }
    return base + " · sem saída nova há " + secsText(quietSecs);
}

void Backend::actTick()
{
    QString t;
    bool st = false;
    if (m_proc && (m_answering || m_loading) && m_turnClock.isValid()) {
        const qint64 pid = m_proc->processId();
        auto ticks = [](qint64 p) -> qint64 {        // utime + stime do processo (campos 14 e 15, depois do nome entre parenteses)
            QFile f(QString("/proc/%1/stat").arg(p));
            if (!f.open(QIODevice::ReadOnly)) return -1;
            const QByteArray s = f.readAll();
            const QList<QByteArray> a = s.mid(s.lastIndexOf(')') + 2).split(' ');
            return a.size() > 12 ? a[11].toLongLong() + a[12].toLongLong() : -1;
        };
        qint64 tot = ticks(pid);
        bool children = false;
        QFile ch(QString("/proc/%1/task/%1/children").arg(pid));
        if (ch.open(QIODevice::ReadOnly))
            for (const QByteArray &c : ch.readAll().simplified().split(' ')) {
                if (c.isEmpty()) continue;
                children = true;
                const qint64 k = ticks(c.toLongLong());
                if (k > 0 && tot >= 0) tot += k;
            }
        const double cores = m_procTicks >= 0 && tot >= m_procTicks ? (tot - m_procTicks) / 100.0 : 1.0;   /* 100 ticks por segundo */
        m_procTicks = tot;
        m_idleSecs = (cores < 0.2 && !children) ? m_idleSecs + 1 : 0;
        t = activityFor(int(m_turnClock.elapsed() / 1000), int(m_lastOut.elapsed() / 1000), cores, children, m_idleSecs, &st);
    } else { m_procTicks = -1; m_idleSecs = 0; }
    if (t != m_actText || st != m_stalled) { m_actText = t; m_stalled = st; emit activityChanged(); }
    updateProgress();
}

// processador (media desde a leitura anterior), memoria em uso e placa de video (carga e memoria, via nvidia-smi)
void Backend::sysTick()
{
    QString cpu, ram;
    QFile st("/proc/stat");
    if (st.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> f = st.readLine().simplified().split(' ');
        if (f.size() >= 9) {
            qint64 tot = 0;
            for (int i = 1; i <= 8; i++) tot += f[i].toLongLong();
            const qint64 idle = f[4].toLongLong() + f[5].toLongLong();
            if (m_cpuTot > 0 && tot > m_cpuTot)
                cpu = QString("CPU %1%").arg(qRound(100.0 * ((tot - m_cpuTot) - (idle - m_cpuIdle)) / (tot - m_cpuTot)));
            m_cpuTot = tot; m_cpuIdle = idle;
        }
    }
    QFile mi("/proc/meminfo");
    if (mi.open(QIODevice::ReadOnly)) {
        double total = 0, avail = 0;
        for (const QByteArray &l : mi.readAll().split('\n')) {
            if (l.startsWith("MemTotal:")) total = l.mid(9).simplified().split(' ').value(0).toDouble();
            else if (l.startsWith("MemAvailable:")) avail = l.mid(13).simplified().split(' ').value(0).toDouble();
        }
        if (total > 0) ram = QString("RAM %1 de %2 GB").arg(qRound((total - avail) / 1048576.0)).arg(qRound(total / 1048576.0));
    }
    if (m_gpuState >= 0 && !m_gpuProc) {
        m_gpuProc = new QProcess(this);
        QProcess *p = m_gpuProc;
        auto done = [this, p](bool ok) {
            if (m_gpuProc != p) return;
            m_gpuProc = nullptr;
            const QList<QByteArray> f = p->readAllStandardOutput().split('\n').value(0).split(',');
            p->deleteLater();
            if (!ok || f.size() < 3) { if (m_gpuState == 0) m_gpuState = -1; m_sysGpu.clear(); return; }
            m_gpuState = 1;
            const QLocale pt(QLocale::Portuguese);
            m_sysGpu = QString("GPU %1% · %2 de %3 GB").arg(f[0].trimmed().toInt())
                           .arg(pt.toString(f[1].trimmed().toDouble() / 1024.0, 'f', 1), pt.toString(f[2].trimmed().toDouble() / 1024.0, 'f', 1));
        };
        connect(p, &QProcess::finished, this, [done](int code, QProcess::ExitStatus s) { done(code == 0 && s == QProcess::NormalExit); });
        connect(p, &QProcess::errorOccurred, this, [done](QProcess::ProcessError e) { if (e == QProcess::FailedToStart) done(false); });
        p->start("nvidia-smi", {"--query-gpu=utilization.gpu,memory.used,memory.total", "--format=csv,noheader,nounits"});
    }
    QStringList parts;
    if (!cpu.isEmpty()) parts << cpu;
    if (!ram.isEmpty()) parts << ram;
    if (!m_sysGpu.isEmpty()) parts << m_sysGpu;
    const QString t = parts.join("   ·   ");
    if (t != m_sysText) { m_sysText = t; emit sysChanged(); }
}

void Backend::cancelPull()
{
    if (!m_pull) return;
    m_pullError = "Download pausado. O que já foi baixado fica guardado: é só continuar.";
    m_pull->terminate();
}

void Backend::autoStartDefault()
{
    if (!m_model.isEmpty() || m_proc)
        return;
    const QString ultimo = m_ui->value("ultimoModelo").toString();
    if (!ultimo.isEmpty()) {
        for (const QVariant &v : m_models) {
            const QVariantMap m = v.toMap();
            if (m["file"].toString() == ultimo && m["usable"].toBool()) {
                start(ultimo);
                return;
            }
        }
    }
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

// `katana coder status <pasta>`: le "arquivos vigentes", os niveis N0..N3, "ignorados por regra" e "rastreador"; nao depende de nenhum
// campo alem desses (o que faltar so some do resumo). Sem "arquivos vigentes" a saida nao e de um indice: retorna false.
bool Backend::parseIndexStatus(const QString &out, QString *text, QString *tip)
{
    static const QRegularExpression reFiles(R"re(arquivos vigentes: (\d+))re");
    static const QRegularExpression reN0(R"re(N0 arvore\s+(\d+) arquivos catalogados \((\d+) sem leitura)re");
    static const QRegularExpression reN1(R"re(N1 texto\s+(\d+) de (\d+) elegiveis \((\d+)%\))re");
    static const QRegularExpression reN2(R"re(N2 simbolos\s+(\d+) de (\d+) elegiveis \((\d+)%\))re");
    static const QRegularExpression reN3(R"re(N3 ligacoes\s+(\d+) de (\d+) elegiveis \((\d+)%\))re");
    static const QRegularExpression reIgn(R"re(ignorados por regra[^:\n]*: (\d+) entradas)re");
    static const QRegularExpression reDisc(R"re((\d+) discovered)re");
    const auto mf = reFiles.match(out);
    if (!mf.hasMatch())
        return false;
    const auto m0 = reN0.match(out), m1 = reN1.match(out), m2 = reN2.match(out), m3 = reN3.match(out), mi = reIgn.match(out);
    const bool running = out.contains(QLatin1String("rastreador: em execucao"));
    const auto md = reDisc.match(out);
    const bool pendingDirs = md.hasMatch() && md.captured(1).toInt() > 0;
    int pct = 100;
    if (m1.hasMatch()) pct = qMin(pct, m1.captured(3).toInt());
    if (m2.hasMatch()) pct = qMin(pct, m2.captured(3).toInt());
    const bool busy = running || pendingDirs || pct < 100;
    *text = busy ? QStringLiteral("índice: indexando %1%").arg(qMin(pct, 99)) : QStringLiteral("índice: pronto");
    QStringList t;
    t << (busy ? QStringLiteral("Índice do código: indexando") : QStringLiteral("Índice do código: pronto"));
    QString files = QStringLiteral("Arquivos: %1").arg(mf.captured(1));
    if (m0.hasMatch() && m0.captured(2).toInt() > 0) files += QStringLiteral(" (%1 sem leitura: binários, grandes, ilegíveis)").arg(m0.captured(2));
    t << files;
    QStringList lv;
    if (m1.hasMatch()) lv << QStringLiteral("texto %1%").arg(m1.captured(3));
    if (m2.hasMatch()) lv << QStringLiteral("símbolos %1%").arg(m2.captured(3));
    if (m3.hasMatch()) lv << QStringLiteral("ligações %1%").arg(m3.captured(3));
    if (!lv.isEmpty()) t << QStringLiteral("Níveis: ") + lv.join(QStringLiteral(", "));
    if (mi.hasMatch()) t << QStringLiteral("Ignorados por regra: %1").arg(mi.captured(1));
    t << (running ? QStringLiteral("Rastreador em execução") : QStringLiteral("Rastreador parado"));
    *tip = t.join('\n');
    return true;
}

// a cada 3 s, so enquanto o agente trabalha (e uma vez ao terminar): pergunta ao motor, sem bloquear a interface; se o comando nao
// existe ou a pasta nao tem indice, o texto some
void Backend::indexPoll()
{
    const bool working = m_answering && m_proc;
    if (!working && !m_idxWasWorking) return;
    m_idxWasWorking = working;
    if (m_katana.isEmpty() || (m_idxProc && m_idxProc->state() != QProcess::NotRunning)) return;
    const QString ws = workspace();
    if (ws.isEmpty()) return;
    auto *p = new QProcess(this);
    m_idxProc = p;
    connect(p, &QProcess::errorOccurred, p, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        if (m_idxProc == p) m_idxProc = nullptr;
        if (!m_idxText.isEmpty()) { m_idxText.clear(); m_idxTip.clear(); emit indexChanged(); }
        p->deleteLater();
    });
    connect(p, &QProcess::finished, p, [this, p, ws](int, QProcess::ExitStatus) {
        if (m_idxProc == p) m_idxProc = nullptr;
        QString text, tip;
        if (ws != workspace() || !parseIndexStatus(QString::fromUtf8(p->readAllStandardOutput()), &text, &tip)) { text.clear(); tip.clear(); }
        if (text != m_idxText || tip != m_idxTip) { m_idxText = text; m_idxTip = tip; emit indexChanged(); }
        p->deleteLater();
    });
    p->setProcessChannelMode(QProcess::SeparateChannels);
    p->start(m_katana, {QStringLiteral("coder"), QStringLiteral("status"), ws});
    QTimer::singleShot(10000, p, [p] { if (p->state() != QProcess::NotRunning) p->kill(); });
}

void Backend::setState(bool ready, bool loading, bool answering)
{
    if ((loading || answering) && !(m_loading || m_answering)) {    // comecou a trabalhar: zera os relogios da atividade
        m_turnClock.restart(); m_lastOut.restart(); m_procTicks = -1; m_idleSecs = 0;
    }
    const bool wasAnswering = m_answering;
    if (answering && !wasAnswering) beginRequest();
    m_ready = ready;
    m_loading = loading;
    m_answering = answering;
    if (wasAnswering && !answering) endRequest();
    if (!loading && !answering && (!m_actText.isEmpty() || m_stalled)) {     // parou de trabalhar: some o aviso de atividade na hora
        m_actText.clear(); m_stalled = false; emit activityChanged();
    }
    emit stateChanged();
    emit thinkingChanged();
    updateInhibit();
}

int Backend::imageSize() const {
    const int n = m_ui->value("imagemTamanho", 1024).toInt();
    return (n == 1024 || n == 1280 || n == 1600 || n == 1920) ? n : 1024;
}
void Backend::setImageSize(int w) {
    if (w != 1024 && w != 1280 && w != 1600 && w != 1920) return;
    if (w == imageSize()) return;
    m_ui->setValue("imagemTamanho", w);
    m_ui->sync();
    emit imageChanged();
}
int Backend::imageSteps() const {
    const int n = m_ui->value("imagemPassos", 4).toInt();
    return (n == 4 || n == 8 || n == 12 || n == 20) ? n : 4;
}
void Backend::setImageSteps(int n) {
    if (n != 4 && n != 8 && n != 12 && n != 20) return;
    if (n == imageSteps()) return;
    m_ui->setValue("imagemPassos", n);
    m_ui->sync();
    emit imageChanged();
}
QString Backend::imageBackend() const {
    const QString b = m_ui->value("imagemBackend", "auto").toString();
    return (b == "cuda" || b == "vulkan" || b == "cpu") ? b : QStringLiteral("auto");
}
void Backend::setImageBackend(const QString &b) {
    if (b != "auto" && b != "cuda" && b != "vulkan" && b != "cpu") return;
    if (b == imageBackend()) return;
    m_ui->setValue("imagemBackend", b);
    m_ui->sync();
    emit imageChanged();
}

bool Backend::keepAwake() const { return m_ui->value("manterAcordado", true).toBool(); }
void Backend::setKeepAwake(bool on)
{
    m_ui->setValue("manterAcordado", on);
    m_ui->sync();
    updateInhibit();
}

// como o caffeinate: enquanto o agente carrega ou responde, pede ao sistema para nao bloquear a tela nem suspender
void Backend::updateInhibit()
{
#ifdef Q_OS_LINUX
    const bool want = keepAwake() && (m_loading || m_answering);
    if (want && !m_inhibit) {
        QString prog;
        QStringList args;
        if (!QStandardPaths::findExecutable("gnome-session-inhibit").isEmpty()) {
            prog = "gnome-session-inhibit";
            args = {"--inhibit", "idle:suspend", "--reason", "Katana: o agente esta trabalhando", "cat"};
        } else if (!QStandardPaths::findExecutable("systemd-inhibit").isEmpty()) {
            prog = "systemd-inhibit";
            args = {"--what=idle:sleep", "--who=Katana", "--why=O agente esta trabalhando", "cat"};
        } else {
            return;
        }
        m_inhibit = new QProcess(this);
        connect(m_inhibit, &QProcess::finished, m_inhibit, &QObject::deleteLater);
        connect(m_inhibit, &QProcess::finished, this, [this] { m_inhibit = nullptr; });
        m_inhibit->start(prog, args);
    } else if (!want && m_inhibit) {
        m_inhibit->disconnect(this);
        m_inhibit->closeWriteChannel();  // o cat le da entrada: fechar a entrada encerra o inibidor sem deixar processo orfao
        QTimer::singleShot(3000, m_inhibit, [p = m_inhibit] { if (p->state() != QProcess::NotRunning) p->terminate(); });
        connect(m_inhibit, &QProcess::finished, m_inhibit, &QObject::deleteLater);
        m_inhibit = nullptr;
    }
#endif
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
    m_ui->setValue("ultimoModelo", m_model);
    m_ui->sync();
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
    p->write("/exit\n");
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
            "KATANA_RETOMAR_TOK", "KATANA_DESKTOP_HOME", "KATANA_DESKTOP_ENV", "KATANA_TRUST",
            "KATANA_SPEC", "KATANA_TOOL_DECL", "KATANA_TEACHER_MODEL", "KATANA_TEACHER_URL", "KATANA_TEACHER_EFFORT", "KATANA_TEACHER_FALLBACK",
            "KATANA_TEACHER_CMD", "KATANA_TEACHER_KEY", "KATANA_TEACHER_VIA", "KATANA_TEACHER_CLI", "KATANA_TEACHER_MAX_TOKENS"};
        for (const QString &k : env.keys())
            if (k.startsWith("KATANA_") && !k.startsWith("KATANA_MCP_") && !manter.contains(k))
                env.remove(k);
    }
    env.insert("KATANA_UI", "1");
    env.insert("NO_COLOR", "1");
    // professor: sem interface; nada do ui.ini liga o professor (so o ambiente herdado, que o desktop nao oferece)
    // especializacoes: todos os .kspec de ~/.katana/specs treinados sobre o modelo escolhido entram na carga
    if (!env.contains("KATANA_SPEC")) {
        const QString specs = specsDir();
        if (!QDir(specs).entryList({"*.kspec"}, QDir::Files).isEmpty())
            env.insert("KATANA_SPEC", specs);
    }
    if (!env.contains("KATANA_THINK"))
        env.insert("KATANA_THINK", "0");
    p->setProcessEnvironment(env);
#ifdef Q_OS_UNIX
    p->setChildProcessModifier([] { ::setpgid(0, 0); });  // grupo proprio: SIGINT so para o agente
#endif
    p->setProgram(m_katana);
    p->setArguments({"agent", path});
    p->setWorkingDirectory(workspace());   // pasta de trabalho do agente (a que o usuario escolheu, ou a atual)
    m_specsActive.clear();      // as linhas de especializacao do motor novo preenchem de novo
    m_specsSkipped.clear();
    emit specsChanged();
    m_outBuf.clear();
    m_errBuf.clear();
    m_errTail.clear();
    m_utf8Tail.clear();
    m_filter.reset();
    m_atLineStart = true;
    connect(p, &QProcess::readyReadStandardOutput, this, &Backend::onStdout);
    connect(p, &QProcess::readyReadStandardError, this, &Backend::onStderr);
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus st) { onFinished(p, code, st); });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && p == m_proc) {
            m_proc = nullptr;
            m_queue.clear(); m_paused = false;
            emit queueChanged();
            setState(false, false, false);
            const QString msg = "Nao foi possivel iniciar o katana (" + m_katana + "): " + p->errorString();
            setTexts({}, msg);
            m_msgs.append("error", msg);
            emit stateChanged();
        }
    });
    m_proc = p;
    p->start();
}

void Backend::onFinished(QProcess *p, int code, QProcess::ExitStatus st)
{
    p->deleteLater();
    if (p != m_proc)
        return;
    m_proc = nullptr;
    // o que sobrou no buffer de stderr (ultima linha sem quebra) tambem conta como motivo
    if (!m_errBuf.trimmed().isEmpty())
        m_errTail << QString::fromUtf8(m_errBuf).trimmed();
    m_errBuf.clear();
    const bool wasLoading = m_loading, wasAnswering = m_answering;
    m_queue.clear(); m_paused = false;
    emit queueChanged();
    m_msgs.finishRunning(QDateTime::currentMSecsSinceEpoch(), true);
    setState(false, false, false);
    // o fim do processo aparece na conversa (nao so numa linha pequena): carregando o modelo ou no meio de um pedido, e erro
    QString msg = st == QProcess::CrashExit ? "O agente travou e foi encerrado pelo sistema" : "O agente terminou (codigo " + QString::number(code) + ")";
    if (wasLoading) msg = "O agente nao conseguiu carregar o modelo. " + msg;
    else if (wasAnswering) msg += " no meio do pedido";
    msg += ".";
    if (!m_errTail.isEmpty())
        msg += "\nUltimas mensagens do motor:\n" + m_errTail.join("\n");
    m_msgs.append("error", msg);
    if (m_dirty)
        saveSession();
    setTexts({}, "O agente terminou. Escolha um modelo para iniciar de novo.");
}

void Backend::shutdown()
{
    if (m_pull) {
        QProcess *q = m_pull;
        m_pull = nullptr;
        q->disconnect(this);
        q->terminate();
        if (!q->waitForFinished(2000)) q->kill();
    }
    if (m_proc) {
        QProcess *p = m_proc;
        m_proc = nullptr;
        p->disconnect(this);
        p->write("/exit\n");
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

bool Backend::enqueue(const QString &text)
{
    const QString t = text.trimmed();
    if (!m_proc || !m_answering || t.isEmpty())
        return false;
    m_queue.append(t);
    emit queueChanged();
    return true;
}

void Backend::removeQueued(int i)
{
    if (i < 0 || i >= m_queue.size())
        return;
    m_queue.removeAt(i);
    if (m_queue.isEmpty())
        m_paused = false;
    emit queueChanged();
}

void Backend::resumeQueue()
{
    m_paused = false;
    emit queueChanged();
    if (m_ready)
        drainQueue();
}

// proxima da fila (quando o agente fica pronto e a fila nao esta pausada)
void Backend::drainQueue()
{
    if (m_queue.isEmpty() || m_paused || !m_ready || !m_proc)
        return;
    const QString n = m_queue.takeFirst();
    emit queueChanged();
    QTimer::singleShot(0, this, [this, n] { if (!send(n)) { m_queue.prepend(n); emit queueChanged(); } });
}

void Backend::stop()
{
    if (!m_proc)
        return;
    m_stopRequested = true;
    if (m_choicePending)
        answerChoice(QString());  // o agente esta parado esperando a resposta: cancela a pergunta
    if (!m_queue.isEmpty()) {  // Parar pausa a fila: o usuario decide se segue
        m_paused = true;
        emit queueChanged();
    }
#ifdef Q_OS_UNIX
    ::kill(-pid_t(m_proc->processId()), SIGINT);  // Ctrl+C no agente: interrompe a resposta
#else
    start(m_model);  // sem SIGINT: mata e reinicia o agente
#endif
}

QString Backend::workspace() const
{
    if (!m_wsReal.isEmpty())
        return m_wsReal;
    const QString saved = m_ui ? m_ui->value("workspace").toString() : QString();
    if (!saved.isEmpty() && QDir(saved).exists())
        return saved;
    return QDir::currentPath();
}

void Backend::undo()
{
    if (m_proc && m_ready)
        writeLine("/undo");
}

// ---- modo professor: a chave fica em ~/.katana/teacher.key (so do dono), nunca no ui.ini ----
static QString teacherKeyPath() { return QDir::home().filePath(".katana/teacher.key"); }
QString Backend::teacherModel() const { return m_ui->value("professorModelo", "").toString(); }
bool Backend::hasTeacherKey() const
{
    if (!qEnvironmentVariableIsEmpty("KATANA_TEACHER_KEY"))
        return true;
    QFile f(teacherKeyPath());
    return f.open(QIODevice::ReadOnly) && !f.readAll().trimmed().isEmpty();
}
void Backend::clearTeacherKey()
{
    QFile::remove(teacherKeyPath());
    if (m_teacher && m_proc && m_ready)
        writeLine("/teacher off");
}
void Backend::setTeacher(const QString &key, const QString &model)
{
    const QString k = key.trimmed(), m = model.trimmed();
    if (!k.isEmpty()) {
        QDir::home().mkpath(".katana");
        QFile f(teacherKeyPath());
        f.remove();
        if (f.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            f.write(k.toUtf8() + "\n");
            f.close();
        }
    }
    if (!m.isEmpty())
        m_ui->setValue("professorModelo", m);
    emit teacherChanged();
    if (m_proc && m_ready)
        writeLine("/teacher " + (teacherModel().isEmpty() ? QString("on") : teacherModel()));        // liga com este modelo (vazio: o padrao do plugin)
}
void Backend::toggleTeacher()
{
    if (!m_proc || !m_ready)
        return;
    if (m_teacher) { writeLine("/teacher off"); return; }
    writeLine("/teacher " + (teacherModel().isEmpty() ? QString("on") : teacherModel()));
}

void Backend::togglePlan()
{
    if (m_proc && m_ready)
        writeLine(m_planMode ? "/plan off" : "/plan");
}

void Backend::approvePlan()
{
    if (!m_proc || !m_ready || !m_planMode)
        return;
    const QString text = QStringLiteral("Plano aprovado: pode executar.");
    setTexts({}, {});
    m_msgs.append("user", text);
    m_sess.mensagens.append({"user", text, {}, QDateTime::currentDateTime().toString(Qt::ISODate)});
    saveSession();
    setState(false, false, true);
    writeLine("/go");
}

void Backend::answerChoice(const QString &textIn)
{
    if (!m_choicePending)
        return;
    QString t = textIn;
    t.replace('\n', ' ').replace('\r', ' ').replace(QChar(1), ' ');
    if (m_proc)
        m_proc->write(QByteArray("\x01" "ANS ") + t.trimmed().toUtf8() + "\n");
    m_choicePending = false;
    m_choiceQ.clear();
    m_choiceOpts.clear();
    emit choiceChanged();
}

void Backend::answerAsk(bool yes)
{
    if (m_proc)
        m_proc->write(yes ? "\x01" "ANS 1\n" : "\x01" "ANS 0\n");
}

bool Backend::setWorkspace(const QString &pathIn)
{
    QString path = pathIn;
    if (path.startsWith("file://"))
        path = QUrl(path).toLocalFile();
    path = QDir::cleanPath(path);
    if (path.isEmpty() || !QDir(path).exists()) {
        setTexts({}, "Pasta nao encontrada.");
        return false;
    }
    if (m_proc && !m_ready) {
        setTexts({}, "Aguarde o agente terminar para trocar de pasta.");
        return false;
    }
    m_ui->setValue("workspace", path);
    m_ui->sync();
    if (m_proc) {  // agente rodando: muda a pasta dele sem recarregar o modelo
        setState(false, false, true);
        writeLine("/cd " + path);
    } else {
        m_wsReal = path;
        emit workspaceChanged();
    }
    return true;
}

void Backend::sendPassword(const QString &pw)
{
    if (m_proc)
        m_proc->write(QByteArray("\x01PASS ") + pw.toUtf8() + "\n");  // direto, sem log nem historico
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
    if (m_paused) {  // mensagem nova digitada a mao retoma a fila
        m_paused = false;
        emit queueChanged();
    }
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
        writeLine("/session " + kp);
        m_bound = kp;
    }
    setTexts({}, {});
    m_speed.clear();
    m_stopRequested = false;
    m_msgs.append("user", text);
    m_sess.mensagens.append({"user", text, {}, QDateTime::currentDateTime().toString(Qt::ISODate)});
    if (m_sess.titulo.isEmpty())
        m_sess.titulo = Sessoes::truncar(Sessoes::umaLinha(text), 80);
    saveSession();
    setState(false, false, true);
    emit sentToAgent(line);
    writeLine(line);
    refreshSessions();
    return true;
}

// ---- geracao de imagem: a imagem anexada vai para `katana render generate`, sem passar pelo agente ----
QString Backend::imageDir() const { return QDir(m_home).filePath("imagens"); }

bool Backend::attachImage(const QString &pathOrUrl)
{
    QString p = pathOrUrl.startsWith("file:") ? QUrl(pathOrUrl).toLocalFile() : pathOrUrl;
    QImageReader r(p);
    if (!QFileInfo(p).isFile() || !r.canRead())
        return false;
    m_attach = QFileInfo(p).absoluteFilePath();
    emit imageChanged();
    return true;
}

void Backend::clearImage()
{
    if (m_attach.isEmpty()) return;
    m_attach.clear();
    emit imageChanged();
}

bool Backend::pasteClipboardImage()
{
    const QImage im = QGuiApplication::clipboard()->image();
    if (im.isNull()) return false;
    QDir().mkpath(imageDir());
    const QString p = QDir(imageDir()).filePath("colada-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + ".png");
    return im.save(p, "PNG") && attachImage(p);
}

// o briefing e o proprio texto digitado ou colado; o que parece briefing vai ao motor como arquivo (--brief), o resto como --nl
bool Backend::looksLikeBrief(const QString &text)
{
    if (text.size() > 600 || text.contains("MATERIAIS") || text.contains("REGRAS"))
        return true;
    for (const QString &l : text.split('\n'))
        if (l.trimmed().startsWith("- \""))
            return true;
    return false;
}

bool Backend::sendImage(const QString &text)
{
    if (m_attach.isEmpty() || m_imgProc || m_katana.isEmpty())
        return false;
    const QString engine = m_imgEngine;
    const QString nome = engine == "flux" ? "FLUX" : "Stable";
    const QString img = m_attach;
    QString prompt = text.trimmed();
    const bool asBrief = !prompt.isEmpty() && looksLikeBrief(prompt);
    if (prompt.isEmpty())
        prompt = QStringLiteral("Fotografia arquitetonica de interiores, fotorrealista, mesma camera, mesma composicao e mesmos materiais da imagem de referencia.");
    // tamanho: multiplos de 64, area do tamanho escolhido no menu (1024x576 a 1920x1080), proporcao da imagem, lados ate 64 acima do nominal
    const int bw = imageSize(), bh = bw * 9 / 16;
    const QSize sz = QImageReader(img).size();
    int W = bw, H = bh;
    if (sz.width() > 0 && sz.height() > 0) {
        const double s = std::sqrt(double(bw) * bh / (double(sz.width()) * sz.height()));
        W = qBound(64, int(std::lround(sz.width() * s / 64.0)) * 64, bw + 64);
        H = qBound(64, int(std::lround(sz.height() * s / 64.0)) * 64, bh + 64);
    }
    QDir().mkpath(imageDir());
    const QString out = QDir(imageDir()).filePath(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + "-" + engine + ".png");
    // o pedido inteiro vai ao motor de prompt do katana (--nl): ele monta o prompt do motor escolhido (dominio, presets, pesos)
    QStringList args{"render", "generate", "--engine", engine, "--init", img, "--strength", "0.35",
                     "--size", QString("%1x%2").arg(W).arg(H), "--out", out};
    QString briefFile;
    if (asBrief) {                                                    // briefing colado: vai como arquivo temporario (removido ao fim)
        briefFile = QDir(QDir::tempPath()).filePath("katana-briefing-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss-zzz") + ".md");
        QFile bf(briefFile);
        if (!bf.open(QIODevice::WriteOnly) || bf.write(prompt.toUtf8()) < 0) {
            m_msgs.append("error", QStringLiteral("Nao foi possivel gravar o briefing temporario."));
            return false;
        }
        bf.close();
        args << "--brief" << briefFile;
    } else {
        args << "--nl" << prompt;
    }
    if (engine == "stable") args << "--lcm" << "--control" << img << "--control-scale" << "1.0";   // as bordas da vista seguram arcos e curvas (ControlNet canny)
    if (engine == "flux") args << "--steps" << QString::number(imageSteps());
    if (QDir(QDir(m_modelsDir).filePath("render")).exists()) args << "--models" << QDir(m_modelsDir).filePath("render");

    if (!m_sess.valida())
        m_sess = Sessoes::nova(m_model);
    m_msgs.append("user", prompt);
    m_sess.mensagens.append({"user", prompt, {}, QDateTime::currentDateTime().toString(Qt::ISODate)});
    if (m_sess.titulo.isEmpty())
        m_sess.titulo = Sessoes::truncar(Sessoes::umaLinha(prompt), 80);
    saveSession();
    refreshSessions();
    m_attach.clear();

    auto *p = new QProcess(this);
    m_imgProc = p;
    p->setProcessChannelMode(QProcess::MergedChannels);
    if (engine == "flux" && imageBackend() != "auto") {           // escolha do menu: CUDA, Vulkan ou CPU; "auto" deixa o katana escolher (CUDA, depois Vulkan, depois CPU)
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("KATANA_BACKEND", imageBackend());
        p->setProcessEnvironment(env);
    }
    connect(p, &QProcess::finished, this, [this, p, out, engine, nome, briefFile](int code, QProcess::ExitStatus st) {
        const QString log = QString::fromUtf8(p->readAll()).trimmed();
        m_imgProc = nullptr;
        p->deleteLater();
        if (!briefFile.isEmpty()) QFile::remove(briefFile);
        if (st == QProcess::NormalExit && code == 0 && QFileInfo::exists(out)) {
            // o motor diz quem processou de fato ("[flux] usado: dit=cuda vae=vulkan"); o cartao mostra CUDA, Vulkan ou CPU
            QString nome2 = engine;
            const auto mt = QRegularExpression(QStringLiteral("\\[flux\\] usado: dit=(\\w+) vae=(\\w+)")).match(log);
            if (mt.hasMatch()) {
                auto rot = [](const QString &s) { return s == "cuda" ? QStringLiteral("CUDA") : s == "vulkan" ? QStringLiteral("Vulkan") : QStringLiteral("CPU"); };
                const QString a = rot(mt.captured(1)), b = rot(mt.captured(2));
                nome2 += "|" + (a == b ? a : QStringLiteral("transformador %1, VAE %2").arg(a, b));
            }
            m_msgs.append("image", out, nome2);
            m_sess.mensagens.append({"image", out, nome2, QDateTime::currentDateTime().toString(Qt::ISODate)});
            saveSession();
        } else if (st == QProcess::NormalExit && code == 3) {   // motor ainda nao existe: aviso amigavel, nao e erro
            m_msgs.append("image", {}, engine, {}, QString("O motor %1 ainda nao esta disponivel nesta versao. Use o Stable por enquanto.").arg(nome));
        } else {
            m_msgs.append("error", QString("A geracao da imagem (%1) falhou (codigo %2):\n%3").arg(nome).arg(code).arg(log.right(600)));
        }
        emit imageChanged();
        reopenAgentAfterImage();
    });
    connect(p, &QProcess::errorOccurred, this, [this, p, nome, briefFile](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        m_imgProc = nullptr;
        if (!briefFile.isEmpty()) QFile::remove(briefFile);
        m_msgs.append("error", QString("Nao foi possivel iniciar o katana para gerar a imagem (%1): %2").arg(nome, p->errorString()));
        p->deleteLater();
        emit imageChanged();
        reopenAgentAfterImage();
    });
    // a placa tem 4 GB e o agente ocupa quase tudo: com o FLUX na GPU o agente sai primeiro (volta ao terminar a imagem)
    if (engine == "flux" && imageBackend() != "cpu" && m_proc && !m_answering) {
        m_reloadModel = m_model;
        detach(m_proc);
        m_proc = nullptr;
        setState(false, false, false);
        setTexts(QStringLiteral("Liberando a placa de video para a imagem..."), {});
        auto *tm = new QTimer(this);
        auto tries = std::make_shared<int>(0);
        connect(tm, &QTimer::timeout, this, [this, tm, tries, p, args] {
            if (m_dying > 0 && ++*tries < 60) return;      // espera o agente sair de fato (libera a VRAM), no maximo ~9 s
            tm->deleteLater();
            setTexts(QStringLiteral("Gerando a imagem na placa de video..."), {});
            p->start(m_katana, args);
            emit imageChanged();
        });
        tm->start(150);
        emit imageChanged();
        return true;
    }
    p->start(m_katana, args);
    emit imageChanged();
    return true;
}

void Backend::reopenAgentAfterImage()
{
    if (m_reloadModel.isEmpty()) return;
    const QString mdl = m_reloadModel;
    m_reloadModel.clear();
    if (!m_proc) start(mdl);
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
    m_queue.clear(); m_paused = false;
    emit queueChanged();
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
    writeLine("/new");
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
    m_msgs.clearPlans();
    m_curPlan = 0;
    for (const Msg &m : s.mensagens) {
        if (m.papel == "plan") {          // o plano do pedido, como estava na ultima gravacao
            m_msgs.restorePlan(m.texto);
            MessageModel::Item it{"plan", {}, {}, {}, m.hora};
            it.plan = m.plano;
            items.append(it);
            continue;
        }
        MessageModel::Item it{m.papel, m.texto, m.nome, {}, m.hora};
        if (m.papel == "tool") {          // sem o resultado (nao e gravado): so o que a sessao guarda
            it.desc = m.desc; it.state = m.estado; it.dur = m.dur; it.plan = m.plano; it.step = m.passo;
        }
        items.append(it);
    }
    m_msgs.setAll(items);
    m_speed.clear();
    m_ctx.clear(); m_ctxUsed = 0; m_ctxTotal = 0;
    bool trocar = false;  // a conversa volta no modelo em que estava
    if (!s.modelo.isEmpty() && s.modelo != m_model) {
        for (const QVariant &v : m_models) {
            const QVariantMap m = v.toMap();
            if (m["file"].toString() == s.modelo && m["usable"].toBool()) { trocar = true; break; }
        }
    }
    if (trocar) {
        start(s.modelo);
    } else if (agentReady && canResume) {
        requestResume(kp);
    } else if (agentReady) {
        setTexts(QStringLiteral("Preparando a conversa..."), {});
        setState(false, false, true);
        m_bound.clear();
        writeLine("/new");
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
    writeLine("/resume " + path);
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

// caminho relativo (achados do agente) e resolvido na pasta de trabalho
QString Backend::resolveFile(const QString &f) const
{
    return QFileInfo(f).isAbsolute() ? f : QDir(workspace()).filePath(f);
}

void Backend::openLocal(const QString &path)
{
    m_lastOpened = path;
    if (!m_testMode)
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void Backend::openPath(const QString &path) { openLocal(resolveFile(path)); }

void Backend::openFolderOf(const QString &path) { openLocal(QFileInfo(resolveFile(path)).absolutePath()); }

void Backend::openSpecsFolder()
{
    QDir().mkpath(specsDir());
    openLocal(specsDir());
}

void Backend::askSummary()
{
    send("Você parou sem me responder. Resuma o que foi feito neste pedido e o que ainda falta fazer.");
}

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
    m_lastOut.restart();
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
        // TOOL <nome>\t<argumentos>[\t<descricao>]
        const QStringList f = args.split('\t');
        QString desc = f.size() >= 3 ? f.mid(2).join(' ').trimmed() : QString();
        QString a = f.size() >= 2 ? cleanArgs(f[1]) : QString();
        if (!desc.isEmpty()) {      // a descricao ja e a linha principal: sai dos argumentos
            QStringList keep;
            for (const QString &p : a.split(" | "))
                if (!p.startsWith("description: ")) keep << p;
            a = keep.join(" | ");
        }
        toolCard(f.value(0), a, desc);
    } else if (name == "TOOLLIVE") {  // TOOLLIVE <pedaco com \\n escapado>: saida parcial do comando em andamento
        QString o = args, r;
        for (int i = 0; i < o.size(); i++) {
            if (o[i] == '\\' && i + 1 < o.size()) { i++; r += o[i] == 'n' ? QChar('\n') : o[i]; }
            else r += o[i];
        }
        m_msgs.setLastToolOut(r, true);
    } else if (name == "TOOLOUT") {  // TOOLOUT <nome>\t<saida com \\n escapado>: resultado da ferramenta no cartao
        const int tab = args.indexOf('\t');
        if (tab >= 0) {
            QString o = args.mid(tab + 1), r;
            for (int i = 0; i < o.size(); i++) {
                if (o[i] == '\\' && i + 1 < o.size()) { i++; r += o[i] == 'n' ? QChar('\n') : o[i]; }
                else r += o[i];
            }
            toolResult(r);
            // Render: "rendered with <motor>: <caminho> (<n> bytes)" + linha "[flux|stable] usado: ..." vira o cartao de imagem logo abaixo da acao
            if (args.left(tab) == "Render") {
                const auto mp = QRegularExpression(QStringLiteral("^rendered with (stable|flux): (.+?) \\(\\d+ bytes\\)"), QRegularExpression::MultilineOption).match(r);
                if (mp.hasMatch() && QFileInfo::exists(mp.captured(2))) {
                    QString nome2 = mp.captured(1);
                    const auto mt = QRegularExpression(QStringLiteral("\\[(?:flux|stable)\\] usado: (?:dit|unet)=(\\w+) vae=(\\w+)")).match(r);
                    if (mt.hasMatch()) {
                        auto rot = [](const QString &x) { return x == "cuda" ? QStringLiteral("CUDA") : x == "vulkan" ? QStringLiteral("Vulkan") : QStringLiteral("CPU"); };
                        const QString a = rot(mt.captured(1)), b = rot(mt.captured(2));
                        nome2 += "|" + (a == b ? a : QStringLiteral("transformador %1, VAE %2").arg(a, b));
                    }
                    m_msgs.append("image", mp.captured(2), nome2);
                    m_sess.mensagens.append({"image", mp.captured(2), nome2, QDateTime::currentDateTime().toString(Qt::ISODate)});
                    saveSession();
                }
            }
        }
    } else if (name == "TOKENS") {  // TOKENS <pos> <ctx>
        const QStringList a = args.split(' ', Qt::SkipEmptyParts);
        if (a.size() >= 2) {
            setCtx(a[0].toInt(), a[1].toInt());
        }
    } else if (name == "COMPACT") {  // COMPACT <antes> <depois>
        const QStringList a = args.split(' ', Qt::SkipEmptyParts);
        if (a.size() >= 2)
            m_msgs.append("notice", "Contexto compactado: " + a[0] + " -> " + a[1] + " tokens");
    } else if (name == "REGROW") {  // a placa de video liberou memoria: o motor se recarrega e retoma a conversa sozinho
        m_msgs.append("notice", "A placa de vídeo liberou memória: recarregando o modelo para ficar mais rápido. A conversa continua.");
    } else if (name == "NEEDPASS") {  // o motor precisa da senha do sudo
        emit passwordRequested(args.trimmed() == "retry");
    } else if (name == "ASK") {  // ASK <tipo>\t<o que>\t<detalhe>: pedido de permissao (confiar na pasta, gravar fora dela, acao)
        const QStringList a = args.split('\t');
        emit askRequested(a.value(0), a.value(1), a.value(2));
    } else if (name == "ASKDONE") {  // a pergunta foi respondida pelo canal (celular): fecha a janela
        emit askClosed();
    } else if (name == "CHANNEL") {  // CHANNEL <servidor>\t<texto>: mensagem que chegou por um canal MCP e virou pedido
        const QStringList a = args.split('\t');
        m_msgs.append("notice", a.value(0) + ": " + a.mid(1).join(' '));
    } else if (name == "PREP") {  // PREP <tokens>: o motor esta calculando o prompt de sistema (so quando muda; depois vem do cache)
        const QStringList a = args.trimmed().split(' ');
        const int total = a.value(0).toInt(), done = a.value(1).toInt();
        const QString pct = total > 0 && a.size() > 1 ? QString::number(done * 100 / total) + "% · " : QString();
        setTexts("Preparando o agente: " + pct + "lendo as instruções (" + a.value(0) + " tokens, só na primeira vez nesta pasta)...", {});
    } else if (name == "FEED") {  // FEED <total> <feito>: o motor esta lendo uma entrada longa (resultado de ferramenta, texto colado)
        const QStringList a = args.trimmed().split(' ');
        const int total = a.value(0).toInt(), done = a.value(1).toInt();
        if (total > 0)
            setTexts("Lendo " + QLocale(QLocale::Portuguese).toString(total) + " tokens: " + QString::number(done * 100 / total) + "%", m_error);
    } else if (name == "TEACHW") {  // TEACHW <bytes>: o professor esta escrevendo as acoes desta resposta (um arquivo longo leva minutos)
        const double kb = args.trimmed().toDouble() / 1024.0;
        setTexts("Professor escrevendo as ações: " + QLocale(QLocale::Portuguese).toString(kb, 'f', 1) + " KB recebidos", m_error);
    } else if (name == "CHOICE") {  // CHOICE <pergunta>\t<opcao>...: pergunta do agente (AskUserQuestion); resposta em answerChoice
        const QStringList a = args.split('\t');
        m_choiceQ = a.value(0);
        m_choiceOpts = a.mid(1);
        m_choicePending = true;
        emit choiceChanged();
    } else if (name == "TODO") {  // TODO <passos com \\n escapado>: lista de tarefas do agente (vazio = sem lista)
        QString r;
        for (int i = 0; i < args.size(); i++) {
            if (args[i] == '\\' && i + 1 < args.size()) { i++; r += args[i] == 'n' ? QChar('\n') : args[i]; }
            else r += args[i];
        }
        m_todo = r.split('\n', Qt::SkipEmptyParts);
        emit todoChanged();
        todoMarker(m_todo);
    } else if (name == "MODE") {  // MODE plan|normal
        m_planMode = args.trimmed() == "plan";
        emit planModeChanged();
        m_msgs.append("notice", m_planMode ? "Modo plano ligado: o agente só lê e propõe um plano; nada é alterado até você aprovar."
                                           : "Modo plano desligado.");
    } else if (name == "TEACHER") {  // TEACHER 0|1 <modelo>
        const bool on = args.trimmed().startsWith("1");
        const QString model = args.trimmed().mid(2).trimmed();
        if (on != m_teacher)
            m_msgs.append("notice", on ? "Professor ligado: " + model + " responde no lugar do modelo local. A conversa e os resultados das ferramentas saem desta máquina; as respostas dele ficam guardadas para treinar o modelo local."
                                       : "Professor desligado: o modelo local volta a responder.");
        m_teacher = on;
        m_ui->setValue("professor", on);
        emit teacherChanged();
    } else if (name == "TEACHERR") {  // TEACHERR <codigo HTTP> <mensagem>: o professor nao respondeu
        m_msgs.append("notice", "O professor não respondeu (" + args.trimmed() + "). Nada foi feito neste passo; mande de novo ou desligue o professor.");
    } else if (name == "UNDO") {  // UNDO <quantos>\t<arquivos>
        const int tab = args.indexOf('\t');
        const QString files = tab < 0 ? QString() : args.mid(tab + 1).trimmed();
        m_msgs.append("notice", files.isEmpty() ? "Nada para desfazer: o agente não alterou arquivos nesta conversa."
                                                : "Alterações desfeitas: " + files);
    } else if (name == "CWD") {  // CWD <pasta>\t<1 se confiavel>
        const QStringList a = args.split('\t');
        m_wsReal = a.value(0);
        m_wsTrusted = a.value(1) == "1";
        if (!m_wsReal.isEmpty()) {
            m_ui->setValue("workspace", m_wsReal);
            m_ui->sync();
        }
        emit workspaceChanged();
    } else if (name == "NOTIFY") {  // NOTIFY <titulo>\t<mensagem>: PushNotification do agente
        const QStringList a = args.split('\t');
        m_msgs.append("notify", a.value(1), a.value(0));
        for (QWindow *w : QGuiApplication::topLevelWindows())     // janela em segundo plano: pisca na barra de tarefas
            if (!w->isActive()) w->alert(0);
    } else if (name == "FILE") {  // FILE <modo>\t<caminho>\t<legenda>: SendUserFile
        const QStringList a = args.split('\t');
        m_msgs.append("file", a.value(1), a.value(0), {}, a.value(2));
    } else if (name == "FINDING") {  // FINDING <arquivo>\t<linha>\t<categoria>\t<resumo>: ReportFindings
        const QStringList a = args.split('\t');
        const QString loc = a.value(1).isEmpty() ? a.value(0) : a.value(0) + ":" + a.value(1);
        m_msgs.append("finding", a.mid(3).join(' '), loc, {}, a.value(2));
    } else if (name == "RESUMING") {  // RESUMING <tokens>: recalculo longo (so na 1a vez; depois o KV vem do disco)
        setTexts("Retomando a sessao: recalculando " + args.trimmed() + " tokens (so na primeira vez, pode levar alguns minutos)...", {});
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
    if (m_status.startsWith("Lendo ")) setTexts({}, m_error);
    if (m_resuming) {  // end of /resume: sem RESUME o motor nao conseguiu; volta ao resumo textual
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
    if (m_answering && !m_stopRequested) {   // o pedido terminou: se o ultimo turno do modelo nao teve texto, avisa
        QString last;
        for (int i = m_msgs.count() - 1; i >= 0; i--) {
            const QString k = m_msgs.get(i)["kind"].toString();
            if (k == "notice" || k == "notify" || k == "image" || k == "file" || k == "finding" || k == "plan") continue;
            last = k;
            break;
        }
        if (last == "tool" || last == "user")
            m_msgs.append("stopped", "O agente parou sem responder.");
    }
    setTexts({}, m_error);
    setState(true, false, false);
    refreshSessions();
    drainQueue();
}

bool Backend::isPlanTool(const QString &name)
{
    return name == "TodoWrite";
}

QString Backend::fmtDuration(qint64 ms)
{
    const qint64 s = (ms + 500) / 1000;
    if (s < 60) return QString::number(s) + " s";
    if (s >= 120) return QString::number((s + 30) / 60) + " min";
    return s % 60 ? QString("1 min %1 s").arg(s - 60) : QStringLiteral("1 min");
}

void Backend::toolCard(const QString &name, const QString &args, const QString &desc)
{
    m_msgs.finishRunning(QDateTime::currentMSecsSinceEpoch());      // uma chamada nova: a anterior sem resultado ja terminou
    const bool meta = isPlanTool(name);
    int plan = 0, step = 0;
    if (m_curPlan && m_msgs.plan(m_curPlan)) {
        plan = m_curPlan;
        step = meta ? -1 : m_msgs.currentStep(m_curPlan);
    }
    const int idx = m_msgs.appendTool(name, args, desc, plan, step);
    if (!meta && m_answering) {
        m_reqActions++;
        if (PlanInfo *p = m_msgs.plan(m_curPlan)) p->acts = m_reqActions;
    }
    if (m_sess.valida()) {
        Msg m{"tool", args, name};
        m.desc = desc;
        m.plano = plan;
        m.passo = step;
        m_sess.mensagens.append(m);
        m_sessOf[idx] = m_sess.mensagens.size() - 1;
        m_dirty = true;
    }
    updateProgress();
}

// resultado da ferramenta que estava rodando: estado (ok ou falhou), duracao e notas do motor separadas
void Backend::toolResult(const QString &result)
{
    const int idx = m_msgs.lastRunningTool();
    if (idx < 0) {
        m_msgs.setLastToolOut(result.isEmpty() ? QStringLiteral("(sem saida)") : result, false);
        return;
    }
    m_msgs.finishTool(idx, result, QDateTime::currentMSecsSinceEpoch());
    const auto &it = m_msgs.item(idx);
    if (m_sessOf.contains(idx) && m_sessOf[idx] < m_sess.mensagens.size()) {
        Msg &m = m_sess.mensagens[m_sessOf[idx]];
        m.estado = it.state;
        m.dur = it.dur;
        m_dirty = true;
    }
    updateProgress();
}

// lista de tarefas do agente mudou (marcador TODO): vira o plano do pedido, com as acoes agrupadas por passo
void Backend::todoMarker(const QStringList &lines)
{
    if (!m_answering)
        return;
    if (lines.isEmpty()) {          // lista apagada
        m_curPlan = 0;
        updateProgress();
        return;
    }
    if (m_curPlan && m_msgs.plan(m_curPlan)) {
        m_msgs.updatePlan(m_curPlan, lines);
        syncPlanSess();
    } else {
        m_curPlan = m_msgs.addPlan(lines, m_reqStart);
        if (PlanInfo *p = m_msgs.plan(m_curPlan)) p->acts = m_reqActions;
        const int run = m_msgs.lastRunningTool();      // a chamada que criou a lista some: o plano a substitui
        if (run >= 0 && isPlanTool(m_msgs.item(run).name)) {
            m_msgs.setToolStep(run, -1);
            if (m_sessOf.contains(run) && m_sessOf[run] < m_sess.mensagens.size()) m_sess.mensagens[m_sessOf[run]].passo = -1;
        }
        m_msgs.insertPlanItem(m_curPlan);
        if (m_sess.valida()) {
            Msg m{"plan", m_msgs.planJson(m_curPlan), {}, QDateTime::currentDateTime().toString(Qt::ISODate)};
            m.plano = m_curPlan;
            m_sess.mensagens.append(m);
            if (PlanInfo *p = m_msgs.plan(m_curPlan)) p->sessIdx = m_sess.mensagens.size() - 1;
            m_dirty = true;
        }
    }
    updateProgress();
}

void Backend::syncPlanSess()
{
    PlanInfo *p = m_msgs.plan(m_curPlan);
    if (!p || p->sessIdx < 0 || p->sessIdx >= m_sess.mensagens.size()) return;
    m_sess.mensagens[p->sessIdx].texto = m_msgs.planJson(p->id);
    m_dirty = true;
}

void Backend::beginRequest()
{
    m_curPlan = 0;
    m_reqStart = QDateTime::currentMSecsSinceEpoch();
    m_reqActions = 0;
    m_sessOf.clear();
    m_progress.clear(); m_stats.clear();
    emit progressChanged();
}

void Backend::endRequest()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    m_msgs.finishRunning(now);
    bool endedPlan = false;
    QString done;
    if (PlanInfo *p = m_msgs.plan(m_curPlan)) {
        m_msgs.endPlan(p->id, now);
        endedPlan = true;
        int d = 0, n = 0;
        for (const PlanStep &s : p->steps) if (s.status != 3) { n++; d += s.status == 2; }
        done = QString("%1 de %2 passos, ").arg(d).arg(n);
        syncPlanSess();
    }
    if (m_reqActions > 0 || !done.isEmpty())
        m_progress = done + QString("%1 %2, %3").arg(m_reqActions).arg(m_reqActions == 1 ? "ação" : "ações", fmtDuration(now - m_reqStart));
    else
        m_progress.clear();
    m_stats = m_progress;
    m_curPlan = 0;
    if (endedPlan && m_dirty) saveSession();       // o fim do plano (hora de termino) entra no arquivo da sessao
    emit progressChanged();
}

// linha de progresso (barra de baixo): enquanto roda "passo 2 de 6: texto · tempo · N ações"; ao terminar o resumo do pedido
void Backend::updateProgress()
{
    QString t, stats;
    if (m_answering && m_reqStart) {
        QStringList parts;
        if (PlanInfo *p = m_msgs.plan(m_curPlan)) {
            int n = 0, cur = 0;
            QString txt;
            for (const PlanStep &s : p->steps) {
                if (s.status == 3) continue;
                n++;
                if (s.status == 1 && !cur) { cur = n; txt = s.title; }
            }
            parts << (cur ? QString("passo %1 de %2: %3").arg(cur).arg(n).arg(txt) : QString("todos os %1 passos feitos").arg(n));
        }
        if (m_reqActions > 0 || !parts.isEmpty()) {
            const QString tail = fmtDuration(QDateTime::currentMSecsSinceEpoch() - m_reqStart) + " · "
                                 + QString("%1 %2").arg(m_reqActions).arg(m_reqActions == 1 ? "ação" : "ações");
            parts << tail;
            t = parts.join(" · ");
            stats = tail;
        }
    } else {
        t = m_progress;
        stats = m_progress;
    }
    if (t != m_progress || stats != m_stats) { m_progress = t; m_stats = stats; emit progressChanged(); }
}

void Backend::emitText(const QByteArray &bytes)
{
    if (bytes.isEmpty())
        return;
    if (m_status.startsWith("Lendo ")) setTexts({}, m_error);      // a leitura longa acabou: a resposta comecou
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
            m_sess.mensagens.append({"assistant", piece, {}, QDateTime::currentDateTime().toString(Qt::ISODate)});
        m_dirty = true;
    }
}

QString Backend::cleanArgs(const QString &in)
{
    const QString s = in.trimmed();
    // valor texto, ou booleano/numero sem aspas (ex.: Edit replace_all, que o Gemma 4 manda como true)
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
    m_lastOut.restart();
    static const QRegularExpression stats(
        R"re(\[entrada: (\d+) tokens, ([\d.]+) tok/s \| resposta: (\d+) tokens, ([\d.]+) tok/s \| contexto (\d+) de (\d+))re");
    int nl;
    while ((nl = m_errBuf.indexOf('\n')) >= 0) {
        const QString l = QString::fromUtf8(m_errBuf.left(nl)).trimmed();
        m_errBuf.remove(0, nl + 1);
        fprintf(stderr, "[katana] %s\n", qPrintable(l));
        if (!l.isEmpty()) {
            m_errTail << l;
            while (m_errTail.size() > 8) m_errTail.removeFirst();
        }
        {   // o bloco [katana: context ...] anexado ao pedido (UIOE): o usuario nao o ve como mensagem; aqui so um aviso discreto
            static const QRegularExpression ctxb(R"re(^\[context: about (\d+) tokens from the project index, (\d+) verified step)re");
            const auto cm = ctxb.match(l);
            if (cm.hasMatch() && m_answering)
                m_msgs.append("notice", "Contexto do katana: ~" + cm.captured(1) + " tokens do índice do projeto, " + cm.captured(2) + " passo(s) verificado(s)");
        }
        {   // banner de carga ("arquitetura=... ctx=131072"): mostra a capacidade do contexto desde o inicio
            static const QRegularExpression banner(R"re(arquitetura=\S+.*\bctx=(\d+))re");
            const auto bm = banner.match(l);
            if (bm.hasMatch() && m_ctxUsed == 0)
                setCtx(0, bm.captured(1).toInt());
        }
        {   // especializacoes aplicadas ou puladas na carga do modelo
            static const QRegularExpression spOk(R"re(^especializacao: (.+) \(\d+ tensores\)$)re");
            static const QRegularExpression spSkip(R"re(^especializacao pulada \((.+?)\): (.+)$)re");
            const auto so = spOk.match(l), ss = spSkip.match(l);
            if (so.hasMatch()) {
                const QString n = QFileInfo(so.captured(1)).fileName();
                if (!m_specsActive.contains(n)) m_specsActive.append(n);
                emit specsChanged();
            } else if (ss.hasMatch()) {
                m_specsSkipped.append(QFileInfo(ss.captured(2)).fileName() + " (" + ss.captured(1) + ")");
                emit specsChanged();
            }
        }
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
