// Backend do katana-desktop: sobe `katana agent <modelo>` com QProcess, traduz a saida
// (marcadores, blocos de ferramenta, estatisticas) e expoe tudo ao QML. Sem servidor HTTP.
#pragma once
#include <QAbstractListModel>
#include <QDir>
#include <QObject>
#include <QElapsedTimer>
#include <QProcess>
#include <QSettings>
#include <QVariantList>
#include <functional>

#include "sessoes.h"

// Passo de um plano e plano do pedido: o plano e a lista de tarefas do agente (marcador TODO) lembrada ao longo do pedido,
// para que cada acao fique sob o passo em que rodou mesmo quando a lista muda
struct PlanStep {
    int id = 0;
    QString title;
    int status = 0;   // 0 pendente, 1 em andamento (o primeiro nao feito), 2 feito, 3 descartado (saiu da lista)
};
struct PlanInfo {
    int id = 0;
    QList<PlanStep> steps;
    int nextStep = 1;
    qint64 t0 = 0, t1 = 0;   // inicio do pedido e fim (0 = ainda rodando), em ms desde a epoca
    int acts = 0;            // acoes do pedido (sem as chamadas de gestao do plano)
    int sessIdx = -1;        // posicao da mensagem "plan" no arquivo da sessao
};

// Lista de mensagens da tela: user, assistant, tool, plan e notice (aviso discreto)
class MessageModel : public QAbstractListModel
{
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int revision READ revision NOTIFY revisionChanged)   // muda (agrupado) a cada alteracao: o QML refaz as visoes de plano
public:
    enum Roles { KindRole = Qt::UserRole + 1, TextRole, NameRole, OutRole, TimeRole, PlanRole, StepRole, VKindRole };
    struct Item {
        QString kind, text, name, out;
        QString time;   // hora ISO da mensagem (vazio = desconhecida)
        // so para tool
        QString desc;       // descricao curta dada pelo modelo (3o campo do marcador TOOL)
        QString note;       // notas do motor ([katana: ...], [plan: ...]) tiradas do resultado: sao para o modelo
        int state = 0;      // 0 desconhecido (sessao antiga), 1 rodando, 2 ok, 3 falhou
        qint64 t0 = 0;      // inicio (ms desde a epoca)
        qint64 dur = -1;    // duracao em ms (-1 = ainda nao terminou ou desconhecida)
        int plan = 0;       // plano do pedido (0 = nenhum); no item "plan", o id do plano
        int step = 0;       // passo do plano (0 = nenhum, -1 = chamada de gestao do plano, escondida)
    };
    static QString showTime(const QString &iso);     // "HH:mm" hoje, "dd/MM HH:mm" em outro dia
    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex &p = {}) const override { return p.isValid() ? 0 : m_items.size(); }
    int count() const { return m_items.size(); }
    int revision() const { return m_rev; }
    QVariant data(const QModelIndex &i, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    QString lastKind() const { return m_items.isEmpty() ? QString() : m_items.last().kind; }
    Q_INVOKABLE QVariantMap get(int i) const;
    void append(const QString &kind, const QString &text, const QString &name = {}, const QString &time = {}, const QString &out = {});
    void appendToLast(const QString &text);
    void setLastToolOut(const QString &out, bool append = false);
    void setAll(const QList<Item> &items);
    void clear() { setAll({}); m_plans.clear(); }
    void clearPlans() { m_plans.clear(); }
    // ferramentas com descricao, tempo e estado
    int appendTool(const QString &name, const QString &args, const QString &desc, int plan, int step);
    int lastRunningTool() const;
    void finishTool(int idx, const QString &result, qint64 nowMs);                // resultado completo (com notas do motor)
    void finishRunning(qint64 nowMs, bool failed = false);                        // fim do pedido: o que ainda roda termina
    void setToolStep(int idx, int step);
    int lastToolIndex() const;
    const Item &item(int i) const { return m_items[i]; }
    static bool resultFailed(const QString &clean);
    static void splitNotes(const QString &raw, QString *clean, QString *note);
    // planos
    int addPlan(const QStringList &todo, qint64 t0);                              // devolve o id
    void updatePlan(int id, const QStringList &todo);                             // reconcilia com a lista nova
    void endPlan(int id, qint64 t1);
    void insertPlanItem(int id);                                                  // item "plan" na conversa
    int currentStep(int id) const;                                                // passo em andamento, ou o ultimo ativo
    Q_INVOKABLE QVariantMap planData(int id) const;
    PlanInfo *plan(int id);
    QString planJson(int id) const;
    void restorePlan(const QString &json);
    int maxPlanId() const;
    void touch();
signals:
    void countChanged();
    void revisionChanged();

private:
    void bump();
    QList<Item> m_items;
    QList<PlanInfo> m_plans;
    int m_rev = 0;
    bool m_revPending = false;
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
    Q_PROPERTY(QString choiceQuestion READ choiceQuestion NOTIFY choiceChanged)
    Q_PROPERTY(QStringList choiceOptions READ choiceOptions NOTIFY choiceChanged)
    Q_PROPERTY(bool choicePending READ choicePending NOTIFY choiceChanged)
    Q_PROPERTY(QStringList todoSteps READ todoSteps NOTIFY todoChanged)   // lista de tarefas do agente: "[x] passo" ou "[ ] passo"
    Q_PROPERTY(bool teacher READ teacher NOTIFY teacherChanged)           // modo professor: um modelo maior responde no lugar do local
    Q_PROPERTY(QString teacherModel READ teacherModel NOTIFY teacherChanged)
    Q_PROPERTY(bool planMode READ planMode NOTIFY planModeChanged)        // modo plano: o agente so le ate o plano ser aprovado
    Q_PROPERTY(QVariantList catalog READ catalog NOTIFY catalogChanged)   // modelos que o aplicativo sabe baixar (katana catalog)
    Q_PROPERTY(QVariantMap machine READ machine NOTIFY catalogChanged)    // memoria, placa, disco livre (katana hw)
    Q_PROPERTY(QString attachedImage READ attachedImage NOTIFY imageChanged)      // imagem anexada ao proximo envio ("" = nenhuma)
    Q_PROPERTY(QString imageEngine READ imageEngine WRITE setImageEngine NOTIFY imageChanged)   // stable, flux ou krea
    Q_PROPERTY(bool imageMode READ imageMode WRITE setImageMode NOTIFY imageChanged)            // modo Imagem: envia para o gerador, nao para o agente
    Q_PROPERTY(bool imageBusy READ imageBusy NOTIFY imageChanged)
    Q_PROPERTY(bool hasUsableModel READ hasUsableModel NOTIFY modelsChanged)
    Q_PROPERTY(QString pullId READ pullId NOTIFY pullChanged)             // modelo sendo baixado ("" = nenhum)
    Q_PROPERTY(double pullFraction READ pullFraction NOTIFY pullChanged)
    Q_PROPERTY(QString pullText READ pullText NOTIFY pullChanged)
    Q_PROPERTY(QString pullError READ pullError NOTIFY pullChanged)
    Q_PROPERTY(QString currentModel READ currentModel NOTIFY currentModelChanged)
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool answering READ answering NOTIFY stateChanged)
    Q_PROPERTY(bool thinking READ thinking NOTIFY thinkingChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY textsChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY textsChanged)
    Q_PROPERTY(QString speedText READ speedText NOTIFY textsChanged)
    // uso da maquina (processador, memoria, placa de video), atualizado a cada 2 s
    Q_PROPERTY(QString sysText READ sysText NOTIFY sysChanged)
    QString sysText() const { return m_sysText; }
    // estado do indice de codigo da pasta de trabalho ("indice: pronto" / "indice: indexando 40%"), lido de `katana coder status <pasta>`
    // enquanto o agente trabalha; vazio quando o comando nao existe ou a pasta nao tem indice
    Q_PROPERTY(QString indexText READ indexText NOTIFY indexChanged)
    Q_PROPERTY(QString indexTip READ indexTip NOTIFY indexChanged)
    // o que o agente esta fazendo enquanto nao responde: tempo do turno, se o processador trabalha, se parece travado
    Q_PROPERTY(QString progressText READ progressText NOTIFY progressChanged)   // "passo 2 de 6: ..." enquanto roda; resumo do pedido ao terminar
    Q_PROPERTY(QString progressStats READ progressStats NOTIFY progressChanged)   // "1 min · 7 ações" (ou o resumo do pedido, ao terminar)
    Q_PROPERTY(QString activityText READ activityText NOTIFY activityChanged)
    Q_PROPERTY(bool stalled READ stalled NOTIFY activityChanged)
    QString activityText() const { return m_actText; }
    bool stalled() const { return m_stalled; }
    // texto e alarme a partir das medidas (funcao pura, usada tambem pelo teste)
public:
    static QString activityFor(int turnSecs, int quietSecs, double cpuCores, bool children, int idleSecs, bool *stalled);
    Q_PROPERTY(QString ctxText READ ctxText NOTIFY textsChanged)
    Q_PROPERTY(int ctxUsed READ ctxUsed NOTIFY textsChanged)
    Q_PROPERTY(int ctxTotal READ ctxTotal NOTIFY textsChanged)
    Q_PROPERTY(QVariantList sessions READ sessions NOTIFY sessionsChanged)
    Q_PROPERTY(QString currentSessionId READ currentSessionId NOTIFY sessionsChanged)
    Q_PROPERTY(QString workspace READ workspace NOTIFY workspaceChanged)
    Q_PROPERTY(bool workspaceTrusted READ workspaceTrusted NOTIFY workspaceChanged)
    Q_PROPERTY(QStringList queue READ queue NOTIFY queueChanged)
    Q_PROPERTY(bool queuePaused READ queuePaused NOTIFY queueChanged)
    Q_PROPERTY(bool hasPendingContext READ hasPendingContext NOTIFY stateChanged)
    Q_PROPERTY(MessageModel *messages READ messages CONSTANT)
    Q_PROPERTY(QString themeOverride READ themeOverride CONSTANT)
    // especializacoes (.kspec) em uso no modelo atual, lidas das linhas que o motor escreve na carga; a pasta e a fonte da verdade
    Q_PROPERTY(QString engineChanges READ engineChanges CONSTANT)          // ultimas mudancas embutidas no motor (katana version --changes)
    Q_PROPERTY(QString engineVersion READ engineVersion CONSTANT)          // `katana version` do executavel que o app usa, mostrado na tela de abertura
    Q_PROPERTY(QStringList specsActive READ specsActive NOTIFY specsChanged)
    Q_PROPERTY(QStringList specsSkipped READ specsSkipped NOTIFY specsChanged)
    Q_PROPERTY(QString specsDir READ specsDir CONSTANT)

public:
    explicit Backend(const QString &katana, const QString &modelsDir, const QString &home,
                     QObject *parent = nullptr);
    ~Backend() override;

    QVariantList models() const { return m_models; }
    QVariantList catalog() const { return m_catalog; }
    QString indexText() const { return m_idxText; }
    QString indexTip() const { return m_idxTip; }
    void setIndexForTest(const QString &t, const QString &tip) { m_idxText = t; m_idxTip = tip; emit indexChanged(); }
    // separa a saida de `katana coder status` em texto curto e resumo; retorna false se a saida nao tem o formato conhecido
    static bool parseIndexStatus(const QString &out, QString *text, QString *tip);
    QString modelsDir() const { return m_modelsDir; }
    QString katanaPath() const { return m_katana; }
    // so para o teste automatico: troca o motor e a pasta de modelos (motor de mentira que fala o mesmo protocolo)
    void testRedirect(const QString &katana, const QString &modelsDir) { m_katana = katana; m_modelsDir = modelsDir; loadModels(); refreshCatalog(); }
    QVariantMap machine() const { return m_machine; }
    bool hasUsableModel() const;
    QString pullId() const { return m_pullId; }
    double pullFraction() const { return m_pullFrac; }
    QString pullText() const { return m_pullText; }
    QString pullError() const { return m_pullError; }
    Q_INVOKABLE void refreshCatalog();
    Q_INVOKABLE void pull(const QString &id);     // baixa um modelo do catalogo (continua um download interrompido)
    Q_INVOKABLE void cancelPull();
    // atalho no menu, na area de trabalho e (GNOME) na dock: so quando roda de um AppImage, perguntado uma vez
    Q_PROPERTY(bool shortcutOffer READ shortcutOffer NOTIFY shortcutChanged)
    bool shortcutOffer() const { return m_scOffer; }
    Q_INVOKABLE void answerShortcut(bool yes);
    static bool installShortcut(const QString &app, const QString &dataDir, const QString &desktopDir, bool dock);
    // so para o teste automatico: oferece o atalho gravando em pastas temporarias, sem mexer na dock nem nas preferencias
    void testShortcut(const QString &app, const QString &dataDir, const QString &desktopDir)
    { m_scApp = app; m_scData = dataDir; m_scDesk = desktopDir; m_scTest = true; m_scOffer = true; emit shortcutChanged(); }
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
    QString progressText() const { return m_progress; }
    QString progressStats() const { return m_stats; }
    static QString fmtDuration(qint64 ms);                 // "35 s", "1 min 20 s", "4 min"
    static bool isPlanTool(const QString &name);           // TodoWrite, todo, Task*: gestao do plano, nao contam como acao
    QString themeOverride() const { return qEnvironmentVariable("KATANA_THEME"); }
    QString sessionsDir() const { return m_sessDir; }

    Q_INVOKABLE void start(const QString &modelFile);
    Q_INVOKABLE bool send(const QString &text);
    QString attachedImage() const { return m_attach; }
    QString imageEngine() const { return m_imgEngine; }
    void setImageEngine(const QString &e) { if ((e == "stable" || e == "flux" || e == "krea") && e != m_imgEngine) { m_imgEngine = e; emit imageChanged(); } }
    bool imageMode() const { return m_imgMode; }
    void setImageMode(bool on) { if (on != m_imgMode) { m_imgMode = on; emit imageChanged(); } }
    bool imageBusy() const { return m_imgProc != nullptr; }
    Q_INVOKABLE bool attachImage(const QString &pathOrUrl);   // anexa uma imagem (caminho ou file://); false se nao for imagem legivel
    Q_INVOKABLE void clearImage();
    Q_INVOKABLE bool sendImage(const QString &text);          // gera a imagem com `katana render generate` (nao passa pelo agente)
    Q_INVOKABLE QString imageDir() const;                     // pasta das imagens geradas
    Q_INVOKABLE void stop();
    Q_INVOKABLE void answerChoice(const QString &text);   // resposta a pergunta do agente (AskUserQuestion); vazia = cancelar
    QString choiceQuestion() const { return m_choiceQ; }
    QStringList choiceOptions() const { return m_choiceOpts; }
    bool choicePending() const { return m_choicePending; }
    QStringList todoSteps() const { return m_todo; }
    bool planMode() const { return m_planMode; }
    Q_INVOKABLE void undo();          // devolve os arquivos alterados pelo ultimo pedido (/undo)
    bool teacher() const { return m_teacher; }
    QString teacherModel() const;
    Q_INVOKABLE void toggleTeacher();                      // liga ou desliga o professor (/teacher); sem chave, pede a chave
    Q_INVOKABLE bool hasTeacherKey() const;                // KATANA_TEACHER_KEY ou ~/.katana/teacher.key
    Q_INVOKABLE void setTeacher(const QString &key, const QString &model);   // grava a chave (vazia = mantem a atual) e o modelo, e liga
    Q_INVOKABLE void clearTeacherKey();
    Q_INVOKABLE void togglePlan();    // liga ou desliga o modo plano (/plan)
    Q_INVOKABLE void approvePlan();   // aprova o plano e manda executar (/go)
    Q_INVOKABLE void answerAsk(bool yes);                 // resposta ao pedido de permissao do agente (confiar na pasta, gravar fora dela...)
    Q_INVOKABLE bool setWorkspace(const QString &path);   // pasta de trabalho do agente (/cd no agente em execucao)
    QString workspace() const;
    bool workspaceTrusted() const { return m_wsTrusted; }
    Q_INVOKABLE bool enqueue(const QString &text);  // mensagem para depois: so enquanto o agente responde
    Q_INVOKABLE void removeQueued(int i);
    Q_INVOKABLE void resumeQueue();                 // depois de Parar a fila fica pausada
    QStringList queue() const { return m_queue; }
    bool queuePaused() const { return m_paused; }
    Q_INVOKABLE void sendPassword(const QString &pw);  // resposta ao pedido de senha do sudo (vazia = cancelar)
    Q_INVOKABLE bool newChat();
    Q_INVOKABLE bool openSession(const QString &id);
    Q_INVOKABLE bool deleteSession(const QString &id);
    Q_INVOKABLE void refreshSessions();
    Q_INVOKABLE void copyText(const QString &text);
    Q_INVOKABLE bool keepAwake() const;  // impede bloqueio de tela/suspensao enquanto o agente trabalha (padrao: ligado)
    Q_INVOKABLE void setKeepAwake(bool on);
    Q_INVOKABLE QString tema() const;
    Q_INVOKABLE void setTema(const QString &t);
    Q_INVOKABLE QString fonte() const;
    Q_INVOKABLE void setFonte(const QString &f);
    Q_INVOKABLE bool uiFlag(const QString &name) const;
    Q_INVOKABLE void setUiFlag(const QString &name, bool v);
    Q_INVOKABLE void autoStartDefault();
    QString engineVersion() const { return m_engineVersion; }
    QString engineChanges() const { return m_engineChanges; }
    QStringList specsActive() const { return m_specsActive; }
    QStringList specsSkipped() const { return m_specsSkipped; }
    QString specsDir() const { return QDir::homePath() + "/.katana/specs"; }
    Q_INVOKABLE void openSpecsFolder();                   // abre a pasta das especializacoes (cria se faltar)
    Q_INVOKABLE void openPath(const QString &path);       // abre um arquivo com o programa padrao do sistema
    Q_INVOKABLE void openFolderOf(const QString &path);   // abre a pasta que contem o arquivo
    Q_INVOKABLE void askSummary();                        // pede ao agente o resumo do que fez e do que falta
    void setTestMode(bool on) { m_testMode = on; }        // so para teste: registra o que abriria em vez de abrir
    QString lastOpened() const { return m_lastOpened; }
    Q_INVOKABLE void clearError();
    void shutdown();
    void injectMarker(const QByteArray &line) { handleMarker(line); }  // so para teste

    static bool usable(const QString &name, QString *note);

signals:
    void modelsChanged();
    void catalogChanged();
    void pullChanged();
    void shortcutChanged();
    void sysChanged();
    void indexChanged();
    void activityChanged();
    void progressChanged();
    void currentModelChanged();
    void stateChanged();
    void thinkingChanged();
    void textsChanged();
    void sessionsChanged();
    void passwordRequested(bool retry);
    void queueChanged();
    void workspaceChanged();
    void askRequested(const QString &kind, const QString &what, const QString &detail);
    void askClosed();
    void choiceChanged();
    void todoChanged();
    void planModeChanged();
    void teacherChanged();
    void teacherKeyNeeded();
    void specsChanged();
    void imageChanged();
    void sentToAgent(const QString &line);  // linha enviada ao agente (para testes)

private:
    void loadModels();
    void launch(const QString &modelFile);
    void maybeLaunch();
    void detach(QProcess *p);
    void onStdout();
    void onStderr();
    void onFinished(QProcess *p, int code, QProcess::ExitStatus st);
    void handleMarker(const QByteArray &line);
    void emitText(const QByteArray &bytes);
    void flushText();
    void assistantText(const QString &t);
    void toolCard(const QString &name, const QString &args, const QString &desc = {});
    void toolResult(const QString &result);
    void todoMarker(const QStringList &lines);
    void beginRequest();
    void endRequest();
    void updateProgress();
    void syncPlanSess();
    void markReady();
    void saveSession();
    void setTexts(const QString &status, const QString &error);
    void setState(bool ready, bool loading, bool answering);
    void writeLine(const QString &line);
    void updateInhibit();
    QProcess *m_inhibit = nullptr;
    static QString cleanArgs(const QString &s);

    QString m_katana, m_modelsDir, m_home, m_sessDir;
    QString m_attach, m_imgEngine = QStringLiteral("stable");
    bool m_imgMode = false;
    QProcess *m_imgProc = nullptr;
    QSettings *m_ui = nullptr;
    QVariantList m_models;
    QProcess *m_proc = nullptr;
    QVariantList m_catalog;
    QVariantMap m_machine;
    QProcess *m_pull = nullptr;
    QString m_scApp, m_scData, m_scDesk;
    void sysTick();
    void indexPoll();
    void actTick();
    QElapsedTimer m_turnClock, m_lastOut;
    QString m_actText;
    // plano do pedido em curso (ver MessageModel): o plano e as acoes que rodaram sob cada passo
    int m_curPlan = 0;
    qint64 m_reqStart = 0;
    int m_reqActions = 0;
    QString m_progress, m_stats;
    QHash<int, int> m_sessOf;       // item da conversa -> mensagem no arquivo da sessao (ferramentas deste pedido)
    bool m_stalled = false;
    qint64 m_procTicks = -1;
    int m_idleSecs = 0;
    QString m_sysText, m_sysGpu;
    QString m_idxText, m_idxTip;
    QProcess *m_idxProc = nullptr;
    bool m_idxWasWorking = false;
    qint64 m_cpuTot = 0, m_cpuIdle = 0;
    int m_gpuState = 0;         // 0 nao tentou, 1 nvidia-smi funciona, -1 sem placa/ferramenta
    QProcess *m_gpuProc = nullptr;
    bool m_scOffer = false, m_scTest = false;
    QString m_pullId, m_pullText, m_pullError;
    QByteArray m_pullBuf;
    double m_pullFrac = 0;
    void onPullLine(const QByteArray &line);
    int m_dying = 0;
    QString m_pendingStart;
    bool m_hasPendingStart = false;
    QString m_model;
    bool m_ready = false, m_loading = false, m_answering = false;
    QString m_status, m_error, m_speed, m_ctx;
    int m_ctxUsed = 0, m_ctxTotal = 0;
    void setCtx(int used, int total);

    QByteArray m_outBuf, m_errBuf, m_utf8Tail;
    QStringList m_errTail;      // ultimas linhas de stderr do agente: o motivo quando ele morre ou nao carrega o modelo
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
    QStringList m_queue;
    bool m_paused = false;
    QString m_wsReal;          // pasta confirmada pelo agente (marcador CWD)
    bool m_wsTrusted = false;
    QString m_choiceQ;            // pergunta pendente do agente (AskUserQuestion)
    QStringList m_choiceOpts;
    bool m_choicePending = false;
    QStringList m_todo;           // passos da lista de tarefas do agente
    bool m_planMode = false;
    bool m_teacher = false;
    void drainQueue();
    QStringList m_specsActive, m_specsSkipped;
    QString m_engineVersion, m_engineChanges;
    bool m_stopRequested = false;     // o usuario apertou Parar neste pedido: terminar sem texto e esperado
    bool m_testMode = false;
    QString m_lastOpened;
    void openLocal(const QString &path);
    QString resolveFile(const QString &f) const;
};
