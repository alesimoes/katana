# solucao de referencia: ss mostra o pid dono do socket que escuta; /proc diz o diretorio dele
pid=$(ss -ltnpH "sport = :$CB_PORT" | sed -n 's/.*pid=\([0-9]*\).*/\1/p' | head -1)
echo "$pid" > answer.txt
readlink "/proc/$pid/cwd" >> answer.txt
