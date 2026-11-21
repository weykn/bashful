# pattern matching (case, [[ == ]], ${x#pat}, ${x//pat/rep}) against tricky patterns
pats=('*' '?' 'a*' '*c' 'a?c' '[abc]*' '[!abc]*' '[^abc]*' '[a-c][a-c]*' '[]x]*' '[!]x]*' '*[0-9]' '[[:digit:]]*' '[[:alpha:]][[:alnum:]_]*'
	'[[:upper:]]*' '*[[:space:]]*' '\**' '*\?' 'a\bc' '[a-]*' '[-a]*' '[' '[abc' 'a[' '*[*' '**a**' '*a*b*c*' '?*?' '[z-a]*' '[[:punct:]]' '[\]]*' 'x*y*z')
strs=('' a abc ac abbc xabc ']x' 'x]' 9 a9 '_a' Abc 'a b' '*star' 'q?' 'abc\' '[' '[abc' 'a[' 'x*y' '-' '!' 'aXbYc' 'xyz' 'xaybzc')
for p in "${pats[@]}"; do
	line=""
	for s in "${strs[@]}"; do
		case $s in $p) line+=1 ;; *) line+=0 ;; esac
		[[ $s == $p ]] && line+=1 || line+=0
	done
	echo "$p $line"
done
for s in "${strs[@]}"; do
	echo "[${s#*b}] [${s##*b}] [${s%b*}] [${s%%b*}] [${s#[a-c]}] [${s%[!a]}] [${s//[abc]/.}] [${s/#?/^}] [${s/%?/$}] [${s//\*/S}] [${s//b*/B}]"
done
long=$(printf 'ab%.0s' {1..300}); echo "${#long} ${long//b/} ${long//a?/X}" | cut -c1-60
export LC_ALL=C.UTF-8
for s in "héllo" "naïve" "abc" "日本語"; do
	case $s in h?llo) echo "u1 $s" ;; ???) echo "u3 $s" ;; [a-z]*) echo "uaz $s" ;; *) echo "uother $s" ;; esac
	echo "${#s} ${s#?} ${s%?} ${s//[a-z]/_}"
done
