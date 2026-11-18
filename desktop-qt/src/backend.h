// Backend do katana-desktop: sobe `katana agent <modelo>` com QProcess, traduz a saida
// (marcadores, blocos de ferramenta, estatisticas) e expoe tudo ao QML. Sem servidor HTTP.
#pragma once
#include <QAbstractListModel>
#include <QObject>
#include <QProcess>
#include <QSettings>
#include <QVariantList>
#include <functional>

#include "sessoes.h"

// Lista de mensagens da tela: user, assistant, tool e notice (aviso discreto)
class MessageModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
public:
    enum Roles { KindRole = Qt::UserRole + 1, TextRole, NameRole };
    struct Item {
        QString kind, text, name;
    };
    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : m_items.size(); }
    int count() const { return m_items.size(); }
    QVariant data(const QModelIndex &i, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    QString lastKind() const { return m_items.isEmpty() ? QString() : m_items.last().kind; }
    Q_INVOKABLE QVariantMap get(int i) const;
    void append(const QString &kind, const QString &text, const QString &name = {});
    void appendToLast(const QString &text);
    void setAll(const QList<Item> &items);
    void clear() { setAll({}); }
signals:
    void countChanged();

private:
    QList<Item> m_items;
};

// Filtro dos blocos de chamada de ferramenta no texto da resposta (com retencao de
// prefixo de etiqueta parcial). Trabalha em bytes: as etiquetas sao ASCII.
class TagFilter
{
public:
    QByteArray feed(const QByteArray &in);
    QByteArray flush();
    void reset()
    {
        m_buf.clear();
        m_closer.clear();
    }

private:
    QByteArray m_buf, m_closer;
};

class Backend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList models READ models NOTIFY modelsChanged)
    Q_PROPERTY(QString currentModel READ currentModel NOTIFY currentModelChanged)
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool answering READ answering NOTIFY stateChanged)
    Q_PROPERTY(bool thinking READ thinking NOTIFY thinkingChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY textsChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY textsChanged)
    Q_PROPERTY(QString speedText READ speedText NOTIFY textsChanged)
    Q_PROPERTY(QString ctxText READ ctxText NOTIFY textsChanged)
    Q_PROPERTY(int ctxUsed READ ctxUsed NOTIFY textsChanged)
    Q_PROPERTY(int ctxTotal READ ctxTotal NOTIFY textsChanged)
    Q_PROPERTY(QVariantList sessions READ sessions NOTIFY sessionsChanged)
    Q_PROPERTY(QString currentSessionId READ currentSessionId NOTIFY sessionsChanged)
    Q_PROPERTY(bool hasPendingContext READ hasPendingContext NOTIFY stateChanged)
    Q_PROPERTY(MessageModel *messages READ messages CONSTANT)
    Q_PROPERTY(QString themeOverride READ themeOverride CONSTANT)

public:
    explicit Backend(const QString &katana, const QString &modelsDir, const QString &home,
                     QObject *parent = nullptr);
    ~Backend() override;

    QVariantList models() const { return m_models; }
    QString currentModel() const { return m_model; }
    bool running() const { return m_proc != nullptr; }
    bool ready() const { return m_ready; }
    bool loading() const { return m_loading; }
    bool answering() const { return m_answering; }
    bool thinking() const { return m_answering && m_msgs.lastKind() != QLatin1String("assistant"); }
    QString statusText() const { return m_status; }
    QString errorText() const { return m_error; }
    QString speedText() const { return m_speed; }
    QString ctxText() const { return m_ctx; }
    int ctxUsed() const { return m_ctxUsed; }
    int ctxTotal() const { return m_ctxTotal; }
    QVariantList sessions() const { return m_sessions; }
    QString currentSessionId() const { return m_sess.id; }
    bool hasPendingContext() const { return !m_pendingCtx.isEmpty(); }
    QString resumeInfo() const { return m_resumeInfo; }   // ultima retomada pelo motor (vazio se nenhuma)
    MessageModel *messages() { return &m_msgs; }
    QString themeOverride() const { return qEnvironmentVariable("KATANA_THEME"); }
    QString sessionsDir() const { return m_sessDir; }
    QString katanaPath() const { return m_katana; }

    Q_INVOKABLE void start(const QString &modelFile);
    Q_INVOKABLE bool send(const QString &text);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void sendPassword(const QString &pw);  // resposta ao pedido de senha do sudo (vazia = cancelar)
    Q_INVOKABLE bool newChat();
    Q_INVOKABLE bool openSession(const QString &id);
    Q_INVOKABLE bool deleteSession(const QString &id);
    Q_INVOKABLE void refreshSessions();
    Q_INVOKABLE void copyText(const QString &text);
    Q_INVOKABLE QString tema() const;
    Q_INVOKABLE void setTema(const QString &t);
    Q_INVOKABLE QString fonte() const;
    Q_INVOKABLE void setFonte(const QString &f);
    Q_INVOKABLE bool uiFlag(const QString &name) const;
    Q_INVOKABLE void setUiFlag(const QString &name, bool v);
    Q_INVOKABLE void autoStartDefault();
    Q_INVOKABLE void clearError();
    void shutdown();
    void injectMarker(const QByteArray &line) { handleMarker(line); }  // so para teste

    static bool usable(const QString &name, QString *note);

signals:
    void modelsChanged();
    void currentModelChanged();
    void stateChanged();
    void thinkingChanged();
    void textsChanged();
    void sessionsChanged();
    void passwordRequested();
    void sentToAgent(const QString &line);  // linha enviada ao agente (para testes)

private:
    void loadModels();
    void launch(const QString &modelFile);
    void maybeLaunch();
    void detach(QProcess *p);
    void onStdout();
    void onStderr();
    void onFinished(QProcess *p);
    void handleMarker(const QByteArray &line);
    void emitText(const QByteArray &bytes);
    void flushText();
    void assistantText(const QString &t);
    void toolCard(const QString &name, const QString &args);
    void markReady();
    void saveSession();
    void setTexts(const QString &status, const QString &error);
    void setState(bool ready, bool loading, bool answering);
    void writeLine(const QString &line);
    static QString cleanArgs(const QString &s);

    QString m_katana, m_modelsDir, m_home, m_sessDir;
    QSettings *m_ui = nullptr;
    QVariantList m_models;
    QProcess *m_proc = nullptr;
    int m_dying = 0;
    QString m_pendingStart;
    bool m_hasPendingStart = false;
    QString m_model;
    bool m_ready = false, m_loading = false, m_answering = false;
    QString m_status, m_error, m_speed, m_ctx;
    int m_ctxUsed = 0, m_ctxTotal = 0;
    void setCtx(int used, int total);

    QByteArray m_outBuf, m_errBuf, m_utf8Tail;
    bool m_atLineStart = true;
    TagFilter m_filter;

    MessageModel m_msgs;
    Session m_sess;
    bool m_dirty = false;
    QString m_pendingCtx;  // resumo da conversa antiga, vai junto da proxima mensagem
    // retomada pelo motor: o agente grava a sessao em <sessoes>/<id>.ksess (/sessao) e a retoma (/retomar)
    QString m_bound;        // arquivo .ksess em que o agente grava agora
    QString m_resumeWant;   // retomada a pedir quando o agente ficar pronto
    QString m_resumeCtx;    // resumo textual de reserva, se a retomada falhar
    bool m_resuming = false, m_resumed = false;
    QString m_resumeInfo;
    QString ksessPath(const QString &id) const;
    void requestResume(const QString &path);
    QVariantList m_sessions;
};
