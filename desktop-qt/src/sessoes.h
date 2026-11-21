// Persistencia das conversas: um arquivo JSON por sessao em <home>/sessoes.
#pragma once
#include <QList>
#include <QString>
#include <QVariantList>

struct Msg {
    QString papel;  // user, assistant, tool
    QString texto;
    QString nome;   // so para tool
    QString hora;   // quando a mensagem foi enviada ou comecou (ISO); vazio nas sessoes antigas
    // so para tool e plan (sessoes antigas nao os tem; o texto de uma "plan" e o JSON do plano)
    QString desc;   // descricao curta da acao, dada pelo modelo
    int estado = 0; // 0 desconhecido, 2 ok, 3 falhou
    qint64 dur = -1;   // duracao em ms (-1 = desconhecida)
    int plano = 0;  // plano do pedido a que pertence (0 = nenhum); em "plan", o id do plano
    int passo = 0;  // passo do plano em que rodou (0 = nenhum, -1 = chamada do proprio plano, escondida)
};

struct Session {
    QString id, titulo, modelo, criada, atualizada;
    QList<Msg> mensagens;
    bool valida() const { return !id.isEmpty(); }
};

namespace Sessoes {
constexpr int maxContexto = 6000;  // caracteres do resumo enviado ao agente ao retomar

bool validId(const QString &id);
QString agoraIso();
QString umaLinha(const QString &s);
QString truncar(const QString &s, int n);
Session nova(const QString &modelo);
bool salvar(const QString &dir, const Session &s);  // escrita atomica (QSaveFile)
bool carregar(const QString &dir, const QString &id, Session *out);
bool apagar(const QString &dir, const QString &id);
// lista (mais recente primeiro) como lista de mapas para o QML
QVariantList listar(const QString &dir, const QString &atual);
// resumo textual da conversa anterior (so usuario e assistente), limitado mantendo o fim
QString montarContexto(const QList<Msg> &ms);
}  // namespace Sessoes
