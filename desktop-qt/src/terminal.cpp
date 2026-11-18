#include "terminal.h"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QProcessEnvironment>
#include <QStandardPaths>

namespace {
const quint32 kAnsi[16] = {0x1e1e24, 0xe06c75, 0x98c379, 0xe5c07b, 0x61afef, 0xc678dd, 0x56b6c2, 0xd4d4d8,
                           0x6b7280, 0xff7b86, 0xb5e890, 0xf0d58c, 0x7dc0ff, 0xd899f0, 0x7fd4de, 0xffffff};

quint32 xterm256(int n)
{
    if (n < 16) return 0xFF000000 | kAnsi[n];
    if (n >= 232) { int v = 8 + (n - 232) * 10; return 0xFF000000 | (v << 16) | (v << 8) | v; }
    n -= 16;
    auto lv = [](int x) { return x == 0 ? 0 : 55 + x * 40; };
    return 0xFF000000 | (lv(n / 36) << 16) | (lv((n / 6) % 6) << 8) | lv(n % 6);
}
QString hex(quint32 c) { return QStringLiteral("#%1").arg(c & 0xFFFFFF, 6, 16, QChar('0')); }
}  // namespace

Terminal::Terminal(const QString &home, QObject *parent) : QObject(parent), m_home(home)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(30);
    connect(&m_timer, &QTimer::timeout, this, &Terminal::render);
    m_lines.append(Line());
}

Terminal::~Terminal() { stop(); }

void Terminal::start()
{
    if (running()) return;
#ifdef Q_OS_WIN
    m_lines.clear(); m_lines.append(Line()); m_r = m_c = 0;
    for (QChar ch : QStringLiteral("Terminal embutido ainda nao disponivel no Windows.")) putChar(ch);
    render();
    return;
#else
    if (QStandardPaths::findExecutable("script").isEmpty()) {
        for (QChar ch : QStringLiteral("Falta o comando 'script' (util-linux) para abrir o terminal.")) putChar(ch);
        render();
        return;
    }
    // readline sem "bracketed paste" (o emulador nao trata essas sequencias)
    const QString rc = QDir(m_home).filePath("inputrc");
    QFile f(rc);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write("$include /etc/inputrc\nset enable-bracketed-paste off\n");
        f.close();
    }
    m_proc = new QProcess(this);
    m_proc->setProcessChannelMode(QProcess::MergedChannels);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("TERM", "xterm-256color");
    env.insert("INPUTRC", rc);
    env.insert("COLORTERM", "truecolor");
    m_proc->setProcessEnvironment(env);
    m_proc->setWorkingDirectory(QDir::homePath());
    connect(m_proc, &QProcess::readyRead, this, [this] { feed(m_proc->readAll()); });
    connect(m_proc, &QProcess::finished, this, [this] {
        for (QChar ch : QStringLiteral("\r\n[processo encerrado]")) putChar(ch);
        scheduleRender();
        emit stateChanged();
    });
    m_proc->start("script", {"-qfec", QStringLiteral("stty cols %1 rows %2; exec bash -i").arg(m_cols).arg(m_rows), "/dev/null"});
    emit stateChanged();
#endif
}

void Terminal::stop()
{
    if (!m_proc) return;
    m_proc->disconnect(this);
    m_proc->terminate();
    if (!m_proc->waitForFinished(500)) m_proc->kill();
    m_proc->deleteLater();
    m_proc = nullptr;
    emit stateChanged();
}

void Terminal::sendKey(const QString &text)
{
    if (running()) m_proc->write(text.toUtf8());
}

void Terminal::paste()
{
    const QString t = QGuiApplication::clipboard()->text();
    if (!t.isEmpty()) sendKey(QString(t).replace("\r\n", "\n").replace('\n', '\r'));
}

void Terminal::clear()
{
    m_lines.clear();
    m_lines.append(Line());
    m_r = m_c = 0;
    render();
}

Terminal::Line &Terminal::cur()
{
    while (m_r >= m_lines.size()) m_lines.append(Line());
    return m_lines[m_r];
}

void Terminal::newline()
{
    m_r++;
    while (m_r >= m_lines.size()) m_lines.append(Line());
    if (m_lines.size() > kMaxLines) {
        const int d = m_lines.size() - kMaxLines;
        m_lines.remove(0, d);
        m_r -= d;
        if (m_r < 0) m_r = 0;
    }
}

void Terminal::putChar(QChar c)
{
    if (m_c >= m_cols) { m_c = 0; newline(); }
    Line &l = cur();
    while (l.size() <= m_c) l.append(Cell());
    Cell cell = m_pen;
    cell.ch = c;
    l[m_c++] = cell;
}

void Terminal::sgr(const QList<int> &p)
{
    if (p.isEmpty()) { m_pen = Cell(); return; }
    for (int i = 0; i < p.size(); i++) {
        const int v = p[i];
        if (v == 0) m_pen = Cell();
        else if (v == 1) m_pen.bold = true;
        else if (v == 22) m_pen.bold = false;
        else if (v == 7) m_pen.inv = true;
        else if (v == 27) m_pen.inv = false;
        else if (v >= 30 && v <= 37) m_pen.fg = 0xFF000000 | kAnsi[v - 30];
        else if (v >= 90 && v <= 97) m_pen.fg = 0xFF000000 | kAnsi[v - 90 + 8];
        else if (v >= 40 && v <= 47) m_pen.bg = 0xFF000000 | kAnsi[v - 40];
        else if (v >= 100 && v <= 107) m_pen.bg = 0xFF000000 | kAnsi[v - 100 + 8];
        else if (v == 39) m_pen.fg = 0;
        else if (v == 49) m_pen.bg = 0;
        else if ((v == 38 || v == 48) && i + 1 < p.size()) {
            quint32 col = 0;
            if (p[i + 1] == 5 && i + 2 < p.size()) { col = xterm256(p[i + 2] & 255); i += 2; }
            else if (p[i + 1] == 2 && i + 4 < p.size()) { col = 0xFF000000 | ((p[i + 2] & 255) << 16) | ((p[i + 3] & 255) << 8) | (p[i + 4] & 255); i += 4; }
            else break;
            (v == 38 ? m_pen.fg : m_pen.bg) = col;
        }
    }
}

void Terminal::csi(const QString &params, QChar fin)
{
    QString ps = params;
    if (!ps.isEmpty() && (ps[0] == '?' || ps[0] == '>' || ps[0] == '=')) return;  // modos privados: ignorados
    QList<int> p;
    for (const QString &s : ps.split(';')) p.append(s.isEmpty() ? -1 : s.toInt());
    auto arg = [&](int i, int def) { return i < p.size() && p[i] > 0 ? p[i] : def; };
    switch (fin.unicode()) {
    case 'm': { QList<int> q; for (int v : p) q.append(v < 0 ? 0 : v); sgr(q); break; }
    case 'A': m_r = qMax(0, m_r - arg(0, 1)); break;
    case 'B': m_r += arg(0, 1); cur(); break;
    case 'C': m_c = qMin(m_cols, m_c + arg(0, 1)); break;
    case 'D': m_c = qMax(0, m_c - arg(0, 1)); break;
    case 'G': m_c = qMin(m_cols - 1, arg(0, 1) - 1); break;
    case 'H': case 'f': {
        const int top = qMax(0, m_lines.size() - m_rows);
        m_r = top + arg(0, 1) - 1;
        m_c = qMin(m_cols - 1, arg(1, 1) - 1);
        cur();
        break;
    }
    case 'J': {
        const int mode = p.isEmpty() || p[0] < 0 ? 0 : p[0];
        if (mode >= 2) { m_lines.clear(); m_lines.append(Line()); m_r = m_c = 0; }
        else if (mode == 0) { Line &l = cur(); if (l.size() > m_c) l.resize(m_c); m_lines.resize(m_r + 1); }
        break;
    }
    case 'K': {
        const int mode = p.isEmpty() || p[0] < 0 ? 0 : p[0];
        Line &l = cur();
        if (mode == 0) { if (l.size() > m_c) l.resize(m_c); }
        else if (mode == 1) { for (int i = 0; i < qMin(m_c + 1, l.size()); i++) l[i] = Cell(); }
        else l.clear();
        break;
    }
    case 'P': { Line &l = cur(); const int n = arg(0, 1); if (m_c < l.size()) l.remove(m_c, qMin(n, l.size() - m_c)); break; }
    case '@': { Line &l = cur(); while (l.size() < m_c) l.append(Cell()); for (int i = 0; i < arg(0, 1); i++) l.insert(m_c, Cell()); break; }
    case 'X': { Line &l = cur(); for (int i = m_c; i < qMin(l.size(), m_c + arg(0, 1)); i++) l[i] = Cell(); break; }
    default: break;
    }
}

void Terminal::feed(const QByteArray &data)
{
    const QString s = m_dec(data);
    for (const QChar c : s) {
        const ushort u = c.unicode();
        switch (m_st) {
        case Normal:
            if (u == 0x1b) m_st = Esc;
            else if (u == '\r') m_c = 0;
            else if (u == '\n') { newline(); m_c = 0; }  // o pty ja traduz \n em \r\n; manter coluna 0 e seguro
            else if (u == '\b') { if (m_c > 0) m_c--; }
            else if (u == '\t') { const int n = 8 - m_c % 8; for (int i = 0; i < n; i++) putChar(' '); }
            else if (u == 7 || u < 32) {}
            else putChar(c);
            break;
        case Esc:
            if (u == '[') { m_st = CsiSt; m_par.clear(); }
            else if (u == ']') m_st = Osc;
            else if (u == '(' || u == ')') m_st = Normal;  // conjunto de caracteres: o proximo byte e ignorado abaixo
            else m_st = Normal;
            break;
        case CsiSt:
            if ((u >= '0' && u <= '9') || u == ';' || u == '?' || u == '>' || u == '=' || u == ' ' || u == '!') m_par += c;
            else { csi(m_par, c); m_st = Normal; }
            break;
        case Osc:  // titulo da janela etc.: descarta ate BEL ou ESC \.
            if (u == 7) m_st = Normal;
            else if (u == 0x1b) m_st = OscEsc;
            break;
        case OscEsc: m_st = Normal; break;
        }
    }
    scheduleRender();
}

void Terminal::scheduleRender()
{
    if (!m_timer.isActive()) m_timer.start();
}

void Terminal::render()
{
    QString out;
    out.reserve(m_lines.size() * 40);
    out += QStringLiteral("<div style=\"white-space:pre\">");
    for (int r = 0; r < m_lines.size(); r++) {
        const Line &l = m_lines[r];
        int n = l.size();
        const bool hasCur = r == m_r;
        if (!hasCur) while (n > 0 && l[n - 1].ch == ' ' && !l[n - 1].bg && !l[n - 1].inv) n--;
        const int lim = hasCur ? qMax(n, m_c + 1) : n;
        quint32 pf = 1, pb = 1; bool pbold = false, pinv = false, open = false;
        for (int i = 0; i < lim; i++) {
            Cell c = i < l.size() ? l[i] : Cell();
            const bool isCur = hasCur && i == m_c;
            const bool inv = c.inv != isCur;
            if (!open || c.fg != pf || c.bg != pb || c.bold != pbold || inv != pinv) {
                if (open) out += QStringLiteral("</span>");
                QString st;
                quint32 fg = c.fg, bg = c.bg;
                if (inv) { const quint32 dfg = 0xFF000000 | 0xd4d4d8, dbg = 0xFF000000 | 0x1b1b21; const quint32 f = fg ? fg : dfg, b = bg ? bg : dbg; fg = b; bg = f; }
                if (fg) st += "color:" + hex(fg) + ";";
                if (bg) st += "background-color:" + hex(bg) + ";";
                if (c.bold) st += "font-weight:bold;";
                out += "<span style=\"" + st + "\">";
                open = true; pf = c.fg; pb = c.bg; pbold = c.bold; pinv = inv;
            }
            const QChar ch = c.ch;
            if (ch == '<') out += QStringLiteral("&lt;");
            else if (ch == '>') out += QStringLiteral("&gt;");
            else if (ch == '&') out += QStringLiteral("&amp;");
            else out += ch;
        }
        if (open) out += QStringLiteral("</span>");
        if (r + 1 < m_lines.size()) out += QStringLiteral("<br>");
    }
    out += QStringLiteral("</div>");
    m_html = out;
    emit changed();
}
