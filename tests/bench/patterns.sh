# pattern matching: case, [[ == ]], [[ =~ ]], globs in ${..}
n1=0 n2=0 n3=0
words=(apple banana cherry date elderberry fig grape honeydew kiwi lemon mango nectarine)
for ((k = 0; k < 2000; k++)); do
	for w in "${words[@]}"; do
		case $w in *an*) ((n1++)) ;; [aeiou]*) ((n2++)) ;; *) ;; esac
		[[ $w == *e ]] && ((n3++))
	done
done
echo "$n1 $n2 $n3"
m=0
for ((i = 0; i < 6000; i++)); do
	s="id-$i-x$((i % 17))"
	if [[ $s =~ ^id-([0-9]+)-x(1[0-6])$ ]]; then ((m += BASH_REMATCH[2])); fi
done
echo "regex: $m"
