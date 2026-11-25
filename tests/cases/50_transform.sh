# ${x@op} transformations, ${!prefix*}, $LINENO, printf %q, time, umask
x="it's" y=$'a\tb' e="" a=(p "q r") z='a\nb\x41\101'
declare -A m=([k]=v)
export ex=1
readonly ro=2
echo "[${x@Q}] [${y@Q}] [${e@Q}] [${unset@Q}] [${a[@]@Q}] [${a@Q}]"
echo "[${x@A}] [${ex@A}] [${ro@A}] [${a[@]@A}] [${a@A}] [${m[@]@A}] [${unset@A}]"
echo "[${x@a}] [${a@a}] [${m@a}] [${ex@a}] [${ro@a}] [${unset@a}]"
echo "[${a[@]@K}] [${m[@]@K}] [${x@K}]"
printf '<%s>' "${a[@]@k}" "${a[@]@K}"; echo
echo "[${x@U}] [${x@u}] [${x@L}] [${z@E}]"
b=('a"b$c`d' $'n\nl' "it's" "")
echo "${b[@]@A}"
f() { local l=1; echo "${l@A}"; }
f
for v in "${a[@]@Q}"; do echo "item $v"; done
set -- "a b" c
echo "[${@@Q}] [${1@Q}] [${*@Q}] ${@@A}"
xa=1 xb=2
echo "[${!x*}] [${!x@}] [${!nonexistent_prefix*}]"
printf '<%s>' "${!x@}"; echo
echo "line $LINENO"
g() {
	echo "in g: $LINENO"
}
g
echo "arith $((LINENO * 10)) $(echo "subst $LINENO")"
eval 'echo "eval $LINENO"'
printf '%q ' "" "a b" 'it'\''s' '$x' '~home' 'a~b' '#c' 'd#' $'tab\there' "café" 'a,b' 'x=y'; echo
TIMEFORMAT='timed %0R'
{ time echo inside; } 2>&1
{ time false; } 2> /dev/null; echo "time status $?"
umask 027; umask; umask -S
(umask 077; umask); umask
umask u=rwx,g=,o=; umask -p
