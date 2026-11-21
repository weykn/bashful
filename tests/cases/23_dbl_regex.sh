# [[ ]]: regex constants with quoted parts, dynamic patterns, backslashes on either side
t() { if [[ $1 =~ $2 ]]; then echo "dyn 1 ${BASH_REMATCH[*]}"; else echo "dyn 0"; fi; }
g() { [[ $1 == $2 ]] && echo "glob 1" || echo "glob 0"; [[ "$1" == "$2" ]] && echo "lit 1" || echo "lit 0"; }
for s in "a.c" "abc" "a+c" "id-12-x13" "x[1]" 'a\b' "foo bar" "(x)" "a*c"; do
	[[ $s =~ ^a.c$ ]] && echo "1 $s" || echo "1n $s"
	[[ $s =~ ^a"."c$ ]] && echo "2 $s" || echo "2n $s"
	[[ $s =~ ^a\.c$ ]] && echo "3 $s" || echo "3n $s"
	[[ $s =~ ^id-([0-9]+)-x(1[0-6])$ ]] && echo "4 ${BASH_REMATCH[1]} ${BASH_REMATCH[2]}" || echo "4n ${#BASH_REMATCH[@]}"
	[[ $s =~ "a+c" ]] && echo "5 $s" || echo "5n"
	[[ $s =~ x\[1\] ]] && echo "6 $s" || echo "6n"
	[[ $s =~ a\\b ]] && echo "7 $s" || echo "7n"
	[[ $s =~ foo\ bar ]] && echo "8 $s" || echo "8n"
	[[ $s =~ (b|c)+ ]] && echo "10 ${BASH_REMATCH[0]}" || echo "10n"
	t "$s" '^a.c$'; t "$s" '[[:alpha:]]+'; t "$s" 'a\\b'
	g "$s" 'a*c'; g "$s" 'a\*c'; g "$s" 'a\b'; g "$s" '?.?'
	[[ $s < b && $s != *x* ]] && echo "lt $s"
done
