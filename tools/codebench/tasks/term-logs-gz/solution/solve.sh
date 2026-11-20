# solucao de referencia: calcula a resposta so com ferramentas de shell
all() { cat logs/app.log; for f in logs/app.log.*.gz; do zcat "$f"; done; }
all | awk '$2=="ERROR"{print $3}' | sort -u | wc -l > answer.txt
all | awk '$2=="ERROR"{print substr($1,12,2)}' | sort | uniq -c | sort -rn | awk 'NR==1{print $2}' >> answer.txt
