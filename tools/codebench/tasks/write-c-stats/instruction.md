faz um programa em C que le numeros da entrada padrao, um por linha, e mostra umas estatisticas. precisa de um Makefile: `make` gera o executavel (o nome vc escolhe) e `make clean` apaga o que o make gerou. a saida é assim, com duas casas decimais:
n=5
soma=15.00
media=3.00
min=1.00
max=5.00
mediana=3.00
os numeros podem ser decimais ou negativos (o que o strtod aceita); linha em branco é ignorada; se a quantidade for par a mediana é a media dos dois do meio. se nao vier nenhum numero escreve `sem dados` no stdout e sai com codigo 1. se tiver uma linha que nao é numero escreve no stderr `linha 3 invalida` (o numero da linha, contando as em branco) e sai com codigo 2. tem que compilar sem nenhum warning com -Wall -Wextra, e nao pode ter limite fixo de tamanho (100000 numeros tem que funcionar).
