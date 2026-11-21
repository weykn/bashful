# indexed and associative arrays
a=()
seed=42
for ((i = 0; i < 700; i++)); do seed=$(((seed * 1103515245 + 12345) % 2147483648)); a+=($((seed % 10000))); done
n=${#a[@]}
for ((i = 0; i < n; i++)); do
	for ((j = 0; j < n - i - 1; j++)); do
		if ((a[j] > a[j + 1])); then t=${a[j]}; a[j]=${a[j + 1]}; a[j + 1]=$t; fi
	done
done
echo "sorted: ${a[0]} ${a[1]} ... ${a[n-1]}"
declare -A count
text="lorem ipsum dolor sit amet consectetur adipiscing elit sed do eiusmod tempor incididunt ut labore et dolore magna aliqua"
for ((k = 0; k < 400; k++)); do
	for w in $text; do count[$w]=$((${count[$w]:-0} + 1)); done
done
keys=0; for w in "${!count[@]}"; do ((keys++)); done
echo "words: $keys lorem=${count[lorem]} et=${count[et]}"
