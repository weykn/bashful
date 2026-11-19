# eval/trap with literal strings, read/mapfile options, BASH_REMATCH, local without splitting
eval 'echo evaled; y=5'; echo "y=$y"
eval "for i in 1 2; do echo e\$i; done"
trap 'echo bye from trap' EXIT
g() { local v="a  b   c"; local w=$v; echo "[$w]"; local n; echo "[${n-unset}]"; }; g
printf 'one two three\nfour\n' | { read -r a b; echo "$a|$b"; read -r c; echo "$c"; }
printf 'abcdef' | { read -n 3 x; echo "$x $?"; read -N 2 y; echo "$y"; }
printf 'a:b:c' | { IFS=: read -r -a arr; echo "${#arr[@]} ${arr[2]}"; }
printf 'l1\nl2\nl3\n' | { mapfile -t lines; echo "${#lines[@]} ${lines[1]}"; }
printf 'x\0y\0' | { read -r -d '' p; read -r -d '' q; echo "$p$q"; }
if [[ "foo-123-bar" =~ ([a-z]+)-([0-9]+) ]]; then echo "${BASH_REMATCH[0]} ${BASH_REMATCH[1]} ${BASH_REMATCH[2]}"; fi
arr=(a b c d); echo $(( ${#arr[@]} * 2 )) ${#arr[@]}
s=hello; (( ${#s} == 5 )) && echo len5
for (( i = ${#arr[@]} - 1; i >= 0; i-- )); do printf '%s' "${arr[i]}"; done; echo
n=3; echo $(( $n + 1 )) $(( n * $n ))
w=(); for ((i=0;i<5;i++)); do w[i]=$((i*i)); done; echo "${w[@]}"
str=abcdef; echo "${str:1:3}" "${str: -2}" "${str:2}" "${str:0:-1}"
d=~; [ "$d" = "$HOME" ] && echo tilde-ok
readarray -t ra <<< $'p\nq'; echo "${ra[*]}"
read -r -t 1 tv <<< "timed"; echo "$tv"
outer() { inner; }; inner() { echo "${FUNCNAME[0]} ${FUNCNAME[1]} ${FUNCNAME[2]} ${#FUNCNAME[@]}"; echo "$FUNCNAME"; }; outer
echo "top:[${FUNCNAME-unset}]"
src="${BASH_SOURCE[0]}"; [ -n "$src" ] && echo has-source
