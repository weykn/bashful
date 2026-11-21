# command substitution and subshells (bash forks for every one of these)
n=0
for ((i = 0; i < 1500; i++)); do
	x=$(echo "$i")
	y=$(printf '%05d' "$x")
	((n += ${#y}))
done
echo "n=$n"
s=0
for ((i = 0; i < 1500; i++)); do (s=$((s + i)); exit 0); s=$((s + 1)); done
echo "s=$s"
up() { echo "${1^^}"; }
r=""
for w in alpha beta gamma delta epsilon; do for ((k = 0; k < 200; k++)); do r=$(up "$w"); done; done
echo "$r"
