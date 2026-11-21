# line-oriented input: while read, IFS splitting, case
tmp=$(mktemp)
for ((i = 0; i < 20000; i++)); do echo "$i,user$((i % 97)),$((i * 7 % 1000)),$((i % 3 == 0 ? 1 : 0))"; done > "$tmp"
sum=0 flagged=0 lines=0
while IFS=, read -r id name score flag; do
	((lines++)); ((sum += score))
	case $name in user1?) ((flagged += flag)) ;; esac
done < "$tmp"
echo "lines=$lines sum=$sum flagged=$flagged"
mapfile -t all < "$tmp"
echo "mapfile: ${#all[@]} last=${all[-1]}"
rm -f "$tmp"
