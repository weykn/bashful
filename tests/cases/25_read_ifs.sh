# read: field splitting with various IFS values, -a, -d, backslashes, last-field remainder
show() { local n=0; for x in "$@"; do printf '[%s]' "$x"; done; echo; }
for line in "a b c" "  a   b  c  " "a,b,,c" "a, b ,c" " , a , b , " "a:b:c:d:e" 'x\ y z' 'tab	sep	here' "" "   " "one" "a,,"; do
	read -r a b c <<< "$line"; show "$a" "$b" "$c"
	IFS=, read -r a b c <<< "$line"; show "$a" "$b" "$c"
	IFS=', ' read -r a b c <<< "$line"; show "$a" "$b" "$c"
	IFS=: read -r a b <<< "$line"; show "$a" "$b"
	IFS= read -r a <<< "$line"; show "$a"
	read a b <<< "$line"; show "$a" "$b"
	IFS=', ' read -ra arr <<< "$line"; show "${arr[@]}"
	IFS=, read -ra arr <<< "$line"; show "${#arr[@]}" "${arr[@]}"
done
printf 'l1 x\nl2\\\ncont\nl3' > /tmp/bashful_r25_$$
while read -r a b; do show "$a" "$b"; done < /tmp/bashful_r25_$$
while read a b; do show "$a" "$b"; done < /tmp/bashful_r25_$$
read -d '' -r all < /tmp/bashful_r25_$$; show "$all"
mapfile -t m < /tmp/bashful_r25_$$; show "${m[@]}"
mapfile m < /tmp/bashful_r25_$$; show "${m[@]}"
mapfile -d x m <<< "axbxc"; show "${m[@]}"
rm -f /tmp/bashful_r25_$$
