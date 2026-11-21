# ${a[expr]} forms compiled to direct slot access
a=(zero one two "three 3" "" five)
i=2
echo "${a[i]} ${a[i+1]} ${a[-1]} ${a[-2]}x ${a[$i]} ${a[ i * 2 ]}"
echo "${#a[i]} ${#a[@]} ${#a[*]} ${a[@]:1:2} ${a[*]:4}"
echo "${a[9]:-dflt} ${a[4]:-empty} ${a[4]-unset} ${a[1]:+alt} ${a[i]#t} ${a[i]%o} ${a[3]// /_} ${a[1]^^}"
for x in ${a[3]}; do echo "split:$x"; done
for x in "${a[@]}"; do echo -n "[$x]"; done; echo
for x in ${a[@]}; do echo -n "<$x>"; done; echo
IFS=,; echo "${a[*]}"; unset IFS
t=${a[1]}; a[1]=${a[2]}; a[2]=$t; echo "${a[1]} ${a[2]}"
a[7]=seven; j=6; echo "${a[j]:-hole} ${a[j+1]} ${#a[@]}"
b=([3]=x [10]=y); k=3; echo "${b[k]} ${b[k+7]} ${!b[@]}"
n=(1 2 3); s=0; for ((q = 0; q < 3; q++)); do v=${n[q]}; ((s += v)); n[q]=$((v * 10)); done; echo "$s ${n[*]}"
e=(); echo "empty: ${#e[@]} [${e[0]}] [${e[@]}]"
x=scalar; echo "${x[0]} ${x[1]:-none}"
unset 'a[3]'; echo "${a[3]:-gone} ${#a[@]}"
(set -u; echo "${a[1]}"; echo "${a[30]}"; echo not-reached) 2>/dev/null; echo "set-u status $?"
