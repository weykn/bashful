# associative arrays (bash key order), keyed array literals, subscripts with expansions, RANDOM seeding
declare -A h
h[apple]=1; h[banana]=2; h["cherry pie"]=3; k=date; h[$k]=4; h[x$k]+=5; h[x$k]+=6
echo "${!h[@]}"; echo "${h[@]}"; echo "${#h[@]}"
echo "${h[apple]} ${h[cherry pie]} ${h["cherry pie"]} ${h[$k]} ${h[xdate]}"
for key in "${!h[@]}"; do printf '%s=%s\n' "$key" "${h[$key]}"; done
unset 'h[banana]'; echo "${!h[*]}"; unset h[$k]; echo "${!h[@]}"
[[ -v h[apple] ]] && echo has-apple; [[ -v h[nope] ]] || echo no-nope
echo "${h[nope]:-dflt} ${h[apple]:+alt} ${#h[apple]} ${h[cherry pie]^^} ${h[cherry pie]/pie/tart}"
((h[cnt]++)); ((h[cnt]+=5)); ((h[$k] = h[cnt] * 2)); echo "cnt=${h[cnt]} date=${h[date]} sum=$(( h[cnt] + h[date] ))"
declare -A m=([one]=1 [two]=2 ["three 3"]=3); echo "${!m[@]} | ${m[@]}"
m+=([four]=4); m[one]+=x; echo "${!m[@]} | ${m[@]}"
declare -A kv=(a 1 b 2 c); echo "${!kv[@]} | ${kv[@]} | [${kv[c]}]"
f() { local -A loc=([in]=side); loc[z]=9; echo "${!loc[@]}"; h[fromf]=yes; }
f; echo "after: ${loc[in]-unset} ${h[fromf]}"
words="the cat the dog the end cat"; declare -A cnt
for w in $words; do ((cnt[$w]++)); done
for w in "${!cnt[@]}"; do echo "$w ${cnt[$w]}"; done
( h[sub]=1; echo "in sub ${#h[@]}" ); echo "outer ${#h[@]} ${h[sub]-none}"
a=([3]=x [1]=y z); echo "${!a[@]} | ${a[@]}"
b=(p q); b+=([7]=r s); echo "${!b[@]} | ${b[@]}"
declare -A big; for ((i=0;i<5000;i++)); do big[k$i]=$i; done; s=""; n=0
for kk in "${!big[@]}"; do ((n++)); [ $n -le 5 ] && s+="$kk "; done; echo "$n $s"
echo "${big[k4999]} ${#big[@]}"
x=([a]=1); echo "${x[@]}"
declare -A e; echo "empty:${#e[@]}:${!e[@]}:"
h=(); echo "reset:${#h[@]}"
declare -A h
r=7; rnd() { r=$(( (r * 1103515245 + 12345) % 2147483648 )); }
for ((i=0;i<20000;i++)); do rnd; k="key$((r % 3000))é$((i%7))"; h[$k]=$i; if ((i % 5 == 0)); then rnd; unset "h[key$((r % 3000))é1]"; fi; done
echo ${#h[@]}
printf '%s\n' "${!h[@]}" | md5sum
printf '%s\n' "${h[@]}" | md5sum
declare -A u=([ü]=1 [ä]=2 [z]=3 ["a b"]=4 [_]=5 [-1]=6 [0]=7); echo "${!u[@]}"; echo "$u ${u}"
RANDOM=7; echo $RANDOM $RANDOM $RANDOM; RANDOM=42; echo $((RANDOM % 100))
unset k; i=2; a2[$i]=x; a2[i+1]=y; a2["k"]=z; echo "${a2[@]}"
