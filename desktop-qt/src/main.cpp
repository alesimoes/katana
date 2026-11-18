// katana-desktop (Qt6/QML): aplicativo nativo de conversa com o agente do katana.
// Sem navegador e sem servidor HTTP: o backend sobe `katana agent <modelo>` com QProcess.
#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

#include "backend.h"
#include "selftest.h"

static QString pick(const QString &given, const QStringList &cands)
{
    if (!given.isEmpty())
        return given;
    for (const QString &c : cands)
        if (QFileInfo::exists(c))
            return QDir::cleanPath(c);
    return cands.first();
}

int main(int argc, char **argv)
{
    bool selftest = false;
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--selftest")
            selftest = true;
    if (selftest && qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND"))
        qputenv("QT_QUICK_BACKEND", "software");  // captura sem GPU (modo offscreen)

    QGuiApplication app(argc, argv);
    app.setApplicationName("katana-desktop");
    app.setApplicationDisplayName("Katana");
    app.setOrganizationName("katana");
    QGuiApplication::setDesktopFileName("katana-desktop");  // casa com katana-desktop.desktop (dock)
    {
        QIcon ic;
        for (int t : {16, 32, 48, 64, 128, 256, 512})
            ic.addFile(QString(":/icons/katana-%1.png").arg(t), QSize(t, t));
        QGuiApplication::setWindowIcon(ic);
    }
    QQuickStyle::setStyle("Basic");

    QCommandLineParser cli;
    cli.setApplicationDescription("Janela de conversa com o agente do katana");
    cli.addHelpOption();
    QCommandLineOption oKatana("katana", "Caminho do executavel katana.", "caminho");
    QCommandLineOption oModels("models", "Pasta dos modelos (.gguf).", "pasta");
    QCommandLineOption oSelf("selftest", "Teste automatico (usa o modelo pequeno e grava capturas).");
    QCommandLineOption oCap("capturas", "Pasta das capturas do selftest.", "pasta");
    QCommandLineOption oModel("selftest-model", "Modelo do selftest.", "arquivo", "Qwen3.6-35B-A3B-UD-Q4_K_M.gguf");
    cli.addOptions({oKatana, oModels, oSelf, oCap, oModel});
    cli.process(app);

    const QString exe = QCoreApplication::applicationDirPath();
    const QString cwd = QDir::currentPath();
    const QString katana = pick(cli.value(oKatana),
                                {exe + "/katana", exe + "/../katana", exe + "/../../katana", exe + "/../../../katana",
                                 cwd + "/katana", cwd + "/../katana"});
    const QString models = pick(cli.value(oModels),
                                {exe + "/modelos", exe + "/../modelos", exe + "/../../modelos", exe + "/../../../modelos",
                                 cwd + "/modelos", cwd + "/../modelos"});
    if (!QFileInfo::exists(katana)) {
        fprintf(stderr, "katana nao encontrado; use --katana CAMINHO\n");
        return 1;
    }

    // pasta de dados: KATANA_DESKTOP_HOME ou ~/.katana/desktop
    QTemporaryDir tmp;
    QString home = qEnvironmentVariable("KATANA_DESKTOP_HOME");
    if (home.isEmpty()) {
        if (selftest) {
            home = tmp.path();  // o selftest nunca toca no ~/.katana do dono
        } else {
            home = QDir::homePath() + "/.katana/desktop";
        }
    }
    QDir().mkpath(home);

    Backend backend(katana, models, home);
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("backend", &backend);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(2); },
                     Qt::QueuedConnection);
    engine.loadFromModule("Katana", "Main");
    if (engine.rootObjects().isEmpty())
        return 2;
    auto *win = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &backend, &Backend::shutdown);

    fprintf(stderr, "katana-desktop | katana: %s | modelos: %s | dados: %s\n", qPrintable(katana), qPrintable(models),
            qPrintable(home));

    if (selftest) {
        QString cap = cli.value(oCap);
        QTimer::singleShot(0, &app, [&] {
            int rc = runSelfTest(backend, win, cap, cli.value(oModel));
            backend.shutdown();
            QCoreApplication::exit(rc);
        });
    } else {
        QTimer::singleShot(0, &backend, &Backend::autoStartDefault);
    }
    return app.exec();
}
