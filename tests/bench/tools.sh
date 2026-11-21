# pipelines of common tools: grep, sort, uniq, cut, tr, wc, head, tail
tmp=$(mktemp -d)
for ((i = 0; i < 3000; i++)); do echo "line $i $((i % 13)) $((i * 31 % 101))"; done > "$tmp/data"
for ((r = 0; r < 60; r++)); do
	a=$(grep -c ' 7 ' "$tmp/data")
	b=$(cut -d' ' -f3 "$tmp/data" | sort -n | uniq -c | wc -l)
	c=$(tr 'a-z' 'A-Z' < "$tmp/data" | head -n 2 | tail -n 1)
	d=$(sort -k4 -n -t' ' "$tmp/data" | tail -1)
done
echo "$a $b [$c] [$d]"
for ((r = 0; r < 200; r++)); do x=$(basename "/a/b/file$r.txt" .txt); y=$(dirname "/a/b/file$r.txt"); done
echo "$x $y"
rm -rf "$tmp"
