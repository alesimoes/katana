# solucao de referencia: responde ao programa pela entrada padrao
n=$(ls data/*.csv | wc -l)
s=$(grep -o -- '-\?[0-9]\+' data/numbers.txt | paste -sd+ | bc)
word=$(awk 'length>m{m=length;l=$0} END{print tolower(l)}' data/poem.txt | awk '{print $1}')
printf '%s\n%s\n%s\n' "$n" "$s" "$word" | python3 vault.py | sed -n 's/.*code=//p' > code.txt
