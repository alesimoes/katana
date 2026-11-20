# solucao de referencia: o primeiro commit cujo diff mexe na comparacao "weight < " e o que quebrou
git log --format=%H -G'weight < ' --reverse -- pricing.py | head -1 > culprit.txt
