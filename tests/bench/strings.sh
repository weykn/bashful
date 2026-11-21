# parameter expansion and string building
s=""
for ((i = 0; i < 4000; i++)); do s+="item$i,"; done
echo "length: ${#s}"
IFS=, read -ra parts <<< "$s"
echo "parts: ${#parts[@]}"
upper=0 rev=""
for p in "${parts[@]}"; do
	t=${p#item}; t=${t%%[05]}
	u=${p^^}; [[ $u == ITEM*7 ]] && ((upper++))
	r=${p//[0-9]/#}
done
echo "upper: $upper last: $r"
word="abcdefghijklmnopqrstuvwxyz"
out=""
for ((k = 0; k < 300; k++)); do
	for ((i = ${#word} - 1; i >= 0; i--)); do out+=${word:i:1}; done
done
echo "${out:0:30} ${#out}"
line="the quick brown fox jumps over the lazy dog"
n=0
for ((k = 0; k < 3000; k++)); do
	x=${line/quick/slow}; x=${x// /_}; y=${x##*_}; z=${x%%_*}
	[[ $y == dog && $z == the ]] && ((n++))
done
echo "matches: $n"
