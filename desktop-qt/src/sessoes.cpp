#include "sessoes.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>

namespace Sessoes {

bool validId(const QString &id)
{
    static const QRegularExpression re(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    return re.match(id).hasMatch();
}

QString agoraIso()
{
    QDateTime n = QDateTime::currentDateTime();
    return n.toOffsetFromUtc(n.offsetFromUtc()).toString(Qt::ISODate);
}

QString umaLinha(const QString &s) { return s.simplified(); }

QString truncar(const QString &s, int n)
{
    const QList<uint> u = s.toUcs4();
    if (u.size() <= n)
        return s;
    return QString::fromUcs4(reinterpret_cast<const char32_t *>(u.constData()), n) + QStringLiteral("...");
}

Session nova(const QString &modelo)
{
    Session s;
    s.id = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    s.modelo = modelo;
    s.criada = s.atualizada = agoraIso();
    return s;
}

bool salvar(const QString &dir, const Session &s)
{
    if (!validId(s.id))
        return false;
    QDir().mkpath(dir);
    QJsonArray arr;
    for (const Msg &m : s.mensagens) {
        QJsonObject o{{"papel", m.papel}, {"texto", m.texto}};
        if (!m.nome.isEmpty())
            o["nome"] = m.nome;
        if (!m.hora.isEmpty())
            o["hora"] = m.hora;
        if (!m.desc.isEmpty())
            o["desc"] = m.desc;
        if (m.estado)
            o["estado"] = m.estado;
        if (m.dur >= 0)
            o["dur"] = double(m.dur);
        if (m.plano)
            o["plano"] = m.plano;
        if (m.passo)
            o["passo"] = m.passo;
        arr.append(o);
    }
    QJsonObject root{{"id", s.id},         {"titulo", s.titulo},         {"modelo", s.modelo},
                     {"criada", s.criada}, {"atualizada", s.atualizada}, {"mensagens", arr}};
    QSaveFile f(QDir(dir).filePath(s.id + ".json"));
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return f.commit();
}

bool carregar(const QString &dir, const QString &id, Session *out)
{
    if (!validId(id))
        return false;
    QFile f(QDir(dir).filePath(id + ".json"));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QJsonParseError err;
    const QJsonDocument d = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !d.isObject())
        return false;
    const QJsonObject o = d.object();
    Session s;
    s.id = id;
    s.titulo = o["titulo"].toString();
    s.modelo = o["modelo"].toString();
    s.criada = o["criada"].toString();
    s.atualizada = o["atualizada"].toString();
    for (const QJsonValue &v : o["mensagens"].toArray()) {
        const QJsonObject m = v.toObject();
        Msg x{m["papel"].toString(), m["texto"].toString(), m["nome"].toString(), m["hora"].toString()};
        x.desc = m["desc"].toString();
        x.estado = m["estado"].toInt();
        x.dur = m.contains("dur") ? qint64(m["dur"].toDouble()) : -1;
        x.plano = m["plano"].toInt();
        x.passo = m["passo"].toInt();
        s.mensagens.append(x);
    }
    *out = s;
    return true;
}

bool apagar(const QString &dir, const QString &id)
{
    if (!validId(id))
        return false;
    QFile f(QDir(dir).filePath(id + ".json"));
    return !f.exists() || f.remove();
}

QVariantList listar(const QString &dir, const QString &atual)
{
    struct Row {
        QVariantMap m;
        qint64 ms;
    };
    QList<Row> rows;
    const QStringList files = QDir(dir).entryList({"*.json"}, QDir::Files);
    for (const QString &fn : files) {
        const QString id = fn.left(fn.size() - 5);
        Session s;
        if (!carregar(dir, id, &s))
            continue;
        qint64 ms = QDateTime::fromString(s.atualizada, Qt::ISODate).toMSecsSinceEpoch();
        QVariantMap m{{"id", id},
                      {"titulo", s.titulo},
                      {"modelo", s.modelo},
                      {"criada", s.criada},
                      {"atualizada", s.atualizada},
                      {"atualizadaMs", ms},
                      {"mensagens", s.mensagens.size()},
                      {"atual", id == atual}};
        rows.append({m, ms});
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) {
        if (a.ms != b.ms)
            return a.ms > b.ms;
        return a.m["id"].toString() > b.m["id"].toString();
    });
    QVariantList out;
    for (const Row &r : rows)
        out.append(r.m);
    return out;
}

QString montarContexto(const QList<Msg> &ms)
{
    QString b;
    for (const Msg &m : ms) {
        if (m.papel == "user")
            b += "Voce: " + umaLinha(m.texto) + "\n";
        else if (m.papel == "assistant")
            b += "Assistente: " + umaLinha(m.texto) + "\n";
    }
    if (b.isEmpty())
        return {};
    QList<uint> u = b.toUcs4();
    if (u.size() > maxContexto)
        b = "..." + QString::fromUcs4(reinterpret_cast<const char32_t *>(u.constData()) + (u.size() - maxContexto), maxContexto);
    return "Contexto da conversa anterior:\n" + b + "\n";
}

}  // namespace Sessoes
