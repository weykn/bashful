# quoted right-hand sides of [[ == ]] match literally; $(<file); exec -a / -l / -c

# [[ x == "$v" ]]: a quoted variable is a string, not a pattern
for v in 'a\b' 'a[c]' 'x\\y' '\' '[' ']' '*' '?' 'a*b' '[abc]' 'two words' ''; do
	w=$v
	if [[ $w == "$v" ]]; then r=eq; else r=ne; fi
	if [[ $w != "$v" ]]; then s=ne; else s=eq; fi
	if [[ xx == "$v" ]]; then t=eq; else t=ne; fi
	printf '%-10s %s %s %s\n' "[$v]" $r $s $t
done
pat='a*'
[[ abc == $pat ]] && echo "unquoted var is a pattern"
[[ abc == "$pat" ]] || echo "quoted var is literal"
[[ 'a*' == "$pat" ]] && echo "quoted var matches itself"
[[ abc == "a"* ]] && echo "quoted part + glob"
[[ 'a\b' == "a\\b" ]] && echo "quoted backslash"
pre='a[' suf=']z'
[[ 'a[1]z' == "$pre"1"$suf" ]] && echo "mixed quoted parts"
long=$(printf 'line [%d] \\ * ?\n' 1 2 3 4 5 6 7 8 9)
copy=$long
[[ $copy == "$long" ]] && echo "long text with specials"

# $(<file) and `<file`
printf 'one\ntwo [x]\n\n\n' > in.txt
x=$(<in.txt)
echo "[$x]"
y=`< in.txt`
echo "[$y]"
n=$(<missing.txt)
echo "missing: $? [$n]"
q="$(<in.txt)"
echo "${#q}"

# exec -a / -l / -c
(exec -a myname bash -c 'echo "0=$0"')
(exec -la myname bash -c 'echo "0=$0"')
(exec -a other bash -c 'echo "0=$0 1=$1"' x y)
export XV=1
(exec -c env) | wc -l
(exec -a)
echo "exec -a without name: $?"

# a function named cat must not take the place of $(<file)
cat() { echo "function cat must not run"; }
z=$( < in.txt )
echo "[$z]"

# -nt / -ot compare modification times to the nanosecond
: > older; : > newer
touch -d '2020-01-01 00:00:00.100' older; touch -d '2020-01-01 00:00:00.200' newer
[[ newer -nt older ]] && echo "newer -nt older"
[[ older -ot newer ]] && echo "older -ot newer"
[[ older -nt newer ]] || echo "older is not -nt newer"
[ newer -nt older ] && echo "[ newer -nt older ]"
cp -p older same
[[ older -nt same || older -ot same ]] || echo "cp -p keeps the time"
