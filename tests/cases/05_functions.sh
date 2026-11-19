greet() { echo "hi $1 ($#)"; return 3; }
greet bob x y; echo "rc=$?"
fact() { local n=$1; if [ $n -le 1 ]; then echo 1; else local p=$(fact $((n-1))); echo $((n*p)); fi; }
fact 5
g=global
f() { local g=local; echo "in $g"; }
f; echo "out $g"
function h { echo "h:$@"; }
h 1 2 3
setg() { g2=set; }; setg; echo $g2
retval() { echo out; return 1; }
if retval; then echo ok; else echo fail; fi
shifty() { while [ $# -gt 0 ]; do echo "arg $1"; shift; done; }
shifty a b c
count() { echo $#; }; count; count ""; count "a b" c
