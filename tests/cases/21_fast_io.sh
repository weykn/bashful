# fast I/O paths: in-memory $(...), shared read buffer, here-string read, in-memory pipelines
d=/tmp/bashful_t21_$$; mkdir -p "$d"; cd "$d"
printf '#!/bin/bash
read x; echo child:$x
' > child.sh; chmod +x child.sh
echo -n "pending-"; x=$(echo inner); echo "[$x]"
y=$(echo a; echo b >&2; echo c) 2>/dev/null; echo "[$y]"
z=$(echo one; /bin/echo two; echo three); echo "[$z]"
w=$(echo p; ls -d / | cat; echo q); echo "[$w]"
v=$(echo s1; { echo s2; echo s3 >&2; } 2>&1; echo s4); echo "[$v]"
u=$(echo o1; x=$(echo i1; /bin/echo i2); echo "got:$x"; echo o2); echo "[$u]"
t=$(echo t1 > tf.txt; echo t2; cat tf.txt); echo "[$t]"
s=$(f() { echo fn; }; f; exit 3; echo no); echo "[$s] $?"
r=$(printf '%s\n' a b c | while read l; do echo "<$l>"; done); echo "[$r]"
q=$(echo bg & wait); echo "[$q]"
[ -t 1 ] && echo tty-yes; tt=$([ -t 1 ] && echo tty || echo notty); echo "[$tt]"
n=$(printf 'x\n\n\n'); echo "[$n]"
m=$(exec /bin/echo execd); echo "[$m]"
l=$(echo e1; exec >&2; echo e2) 2>/dev/null; echo "[$l]"
for i in 1 2 3; do echo -n "$i:"; k=$(echo $((i*i))); echo -n "$k "; done; echo
printf 'l1\nl2\nl3\nl4\nl5\nl6\nl7\n' > in.txt
{ read a; read b; head -n1; read c; x=$(cat /dev/null); read d; cat; } < in.txt
echo "$a $b $c $d"
{ read a; /usr/bin/head -n1; read b; ( read c; echo sub:$c ); read d; echo "$a|$b|$d"; } < in.txt
while read -r l; do echo got $l; [ "$l" = l3 ] && { read -r skip; echo skipped $skip; }; done < in.txt
exec 3< in.txt; read -u 3 q; read r < in.txt; read -u 3 q2; echo "$q $r $q2"; exec 3<&-
{ read a; mapfile -n 2 arr; read b; echo "$a ${arr[*]} $b"; } < in.txt
{ read a; ./child.sh; read b; echo "$a $b"; } < in.txt
IFS=, read -ra p <<< "a,b,,c"; echo "${#p[@]} ${p[*]}"
read x y <<< "  one two three  "; echo "[$x][$y]"
read -r z <<< 'a\b'; echo "$z"; read z <<< 'a\b'; echo "$z"
read -d '' w <<< "multi
line"; echo "[$w]"
read q <<< ""; echo "st=$? [$q]"
read -n 2 c <<< "hello"; echo $c
read -u 0 m <<< "via-u"; echo $m
v="x y"; read a b <<< $v; echo "$b"
read <<< "reply here"; echo "$REPLY"
f() { for ((i=0;i<3;i++)); do echo in$i; return 3; done > r1.txt; }
f; echo "after f $?"
for x in a b; do if [ $x = a ]; then echo y; fi > r2.txt; break; done 2>/dev/null; echo "after loop"
g() { while true; do echo w; break; done > r3.txt; echo g-out; }; g
cat r1.txt r2.txt r3.txt
echo "a b c" | tr a-z A-Z
printf 'x\ny\nz\n' | { read a; read b; echo "$a$b"; cat; }
printf 'l1\nl2\nl3\n' | head -n1; printf 'l1\nl2\n' | { head -n1; echo after; cat; }
echo hi | /usr/bin/tr h H
echo hi | { read x; /bin/cat; echo "x=$x"; }
printf '1\n2\n3\n' | { read a; /usr/bin/head -n1; read c; echo "$a/$c"; }
echo data | { read -t 0 && echo ready; cat; }
echo foo | [ -t 0 ] && echo tty || echo notty
printf 'q\nw\n' | while read l; do echo "<$l>"; done
echo "in" | { cat < /dev/null; cat; }
x=$(printf 'a\nb\n' | sort -r | tr '\n' ,); echo "$x"
printf 'k1 v1\nk2 v2\n' | while read k v; do echo "$v" | tr v V; done
echo nested | { echo "pre" | cat; cat; }
printf 'a\0b\n' | wc -c
echo "z" | mapfile -t arr; echo "${#arr[@]}"
seq 3 | { mapfile -t m; echo "${m[*]}"; }
echo hello | grep -c l
yes | head -n 3
seq 1 100000 | { read a; /usr/bin/wc -l; }
seq 1 300000 | { read a; /usr/bin/head -n1; }
seq 1 300000 | /usr/bin/tail -n1
x=$(seq 1 200000 | /usr/bin/sort -n | tail -n 1); echo $x
for ((i=0;i<3;i++)); do echo $i; done > o.txt; if true; then echo y; fi >> o.txt; cat o.txt
cd /; rm -rf "$d"
