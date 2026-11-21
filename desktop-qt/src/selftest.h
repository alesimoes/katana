#pragma once
#include <QString>

class Backend;
class QQuickWindow;

// Teste de ponta a ponta com o modelo pequeno; devolve 0 se tudo passou
// Teste com motor de mentira (sem modelo): marcadores NOTIFY/FILE/FINDING, "parou sem responder" e especializacoes
int runStandinTest(Backend &backend, QQuickWindow *win, const QString &capDir);
int runSelfTest(Backend &backend, QQuickWindow *win, const QString &capDir, const QString &model);
