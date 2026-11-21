# metric

    python3 metric.py NOME VALOR

Envia uma metrica para o pingd (linha `NOME VALOR`, resposta `OK` ou `ERR motivo`).

Precisa do pingd rodando e escutando em `127.0.0.1:9125` (ou no endereco `host:porta` da variavel `PINGD_ADDR`).
Sem ele o script avisa que o pingd nao esta acessivel e sai com codigo 1.
