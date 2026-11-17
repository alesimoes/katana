#pragma once
#include <QString>

class Backend;
class QQuickWindow;

// Teste de ponta a ponta com o modelo pequeno; devolve 0 se tudo passou
int runSelfTest(Backend &backend, QQuickWindow *win, const QString &capDir, const QString &model);
