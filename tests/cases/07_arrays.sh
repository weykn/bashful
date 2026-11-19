a=(one two "three four"); echo ${#a[@]} "${a[1]}" "${a[@]}"
a+=(five); echo ${#a[@]} ${a[3]}
a[1]=TWO; echo "${a[@]}"
for e in "${a[@]}"; do echo "<$e>"; done
echo "${!a[@]}"
echo "${a[@]:1:2}"
b=(); b[3]=x; echo ${#b[@]} "${b[3]}"
echo "${a[-1]}"
n=0; arr=(5 6 7); echo "${arr[$n]} ${arr[n+1]}"
