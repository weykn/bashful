a=1; b="two words"
echo $a "$b" '$a' "\$a" a\ b
echo "${a}x" ${b} "${b}"
set -- p q "r s"
echo $# "$1" "$@"
for x in "$@"; do echo "[$x]"; done
for x in $@; do echo "<$x>"; done
echo "$*"; echo $0 >/dev/null
false; echo $?
true && echo yes || echo no
false && echo yes || echo no
! false && echo negated
echo a; echo b; echo c
