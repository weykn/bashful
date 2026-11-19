# common real-world idioms
set -euo pipefail
log() { printf '[%s] %s\n' "$1" "${*:2}"; }
die() { log ERR "$@" >&2; exit 1; }
log INFO "starting" with args
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
printf '%s\n' banana apple cherry apple > "$tmp/fruits.txt"
sort -u "$tmp/fruits.txt" | while IFS= read -r f; do echo "fruit: $f"; done
count=$(wc -l < "$tmp/fruits.txt"); echo "count=$((count))"
declare -a parts; IFS=, read -r -a parts <<< "x,y,,z"; echo "${#parts[@]}:${parts[*]}"
path=/usr/local/lib/libfoo.so.1.2
echo "${path##*/} ${path%/*} ${path#/usr} ${path%%.*} ${path/lib/LIB} ${path//o/0}"
name="Hello World"; echo "${name,,} ${name^^} ${#name}"
case "$path" in *.so*) echo shared;; *.a) echo static;; *) echo other;; esac
for f in "$tmp"/*.txt; do echo "file $(basename "$f" .txt)"; done
cat > "$tmp/cfg" <<CFG
key1=value one
# comment
key2 = value2
CFG
while IFS='=' read -r k v; do [[ $k =~ ^[[:space:]]*# || -z $k ]] && continue; k=${k// /}; v=${v# }; echo "<$k> <$v>"; done < "$tmp/cfg"
max() { local m=$1; shift; for x in "$@"; do (( x > m )) && m=$x; done; echo "$m"; }
echo "max=$(max 3 9 2 7)"
arr=(one "two words" three); for a in "${arr[@]}"; do echo "- $a"; done
echo "${arr[@]:1}"; echo "${!arr[@]}"
s="a b  c"; set -- $s; echo "$# $2"
fib() { local n=$1; if (( n < 2 )); then echo "$n"; else echo $(( $(fib $((n-1))) + $(fib $((n-2))) )); fi; }
echo "fib10=$(fib 10)"
x=${UNSET_VAR:-default}; echo "$x"; : "${Y:=assigned}"; echo "$Y"
if ! grep -q zzz "$tmp/fruits.txt"; then echo "no zzz"; fi
n=0; until (( n >= 3 )); do n=$((n+1)); done; echo "n=$n"
printf '%-6s|%6s|%06.2f\n' ab cd 3.14159
str="key: value"; [[ $str == key:* ]] && echo prefix
echo $(( 2**10 )) $(( 17 % 5 )) $(( -17 / 5 )) $(( 1 << 4 ))
{ echo grouped; echo out; } > "$tmp/g"; cat "$tmp/g"
result=$( { echo stdout; echo stderr >&2; } 2>&1 ); echo "$result" | sort
exec 3> "$tmp/fd3"; echo via-fd3 >&3; exec 3>&-; cat "$tmp/fd3"
ls "$tmp" | sort | tr '\n' ' '; echo
log INFO done
