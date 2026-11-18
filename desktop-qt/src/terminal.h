// Terminal embutido do katana-desktop: shell real em pty (via `script`), emulador simples de tela
// (cores SGR, cursor, edicao de linha do readline) e saida em HTML para o QML.
#pragma once
#include <QObject>
#include <QProcess>
#include <QStringDecoder>
#include <QTimer>
#include <QVector>

class Terminal : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString html READ html NOTIFY changed)
    Q_PROPERTY(bool running READ running NOTIFY stateChanged)
    Q_PROPERTY(int cols READ cols CONSTANT)
public:
    explicit Terminal(const QString &home, QObject *parent = nullptr);
    ~Terminal() override;
    QString html() const { return m_html; }
    bool running() const { return m_proc && m_proc->state() != QProcess::NotRunning; }
    int cols() const { return m_cols; }
    Q_INVOKABLE void start();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void sendKey(const QString &text);  // bytes ja prontos (tecla, sequencia ESC, Ctrl+letra)
    Q_INVOKABLE void paste();
    Q_INVOKABLE void clear();
signals:
    void changed();
    void stateChanged();

private:
    struct Cell {
        QChar ch = ' ';
        quint32 fg = 0, bg = 0;  // 0 = padrao; senao 0xFF000000 | rgb
        bool bold = false, inv = false;
    };
    using Line = QVector<Cell>;
    void feed(const QByteArray &data);
    void putChar(QChar c);
    void csi(const QString &params, QChar fin);
    void sgr(const QList<int> &p);
    void newline();
    Line &cur();
    void render();
    void scheduleRender();

    QString m_home;
    QProcess *m_proc = nullptr;
    QStringDecoder m_dec{QStringDecoder::Utf8};
    QVector<Line> m_lines;
    int m_r = 0, m_c = 0, m_cols = 120, m_rows = 30;
    Cell m_pen;
    enum St { Normal, Esc, CsiSt, Osc, OscEsc } m_st = Normal;
    QString m_par;
    QString m_html;
    QTimer m_timer;
    static constexpr int kMaxLines = 3000;
};
