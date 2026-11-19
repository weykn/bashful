set -e
echo start
false || echo handled
if false; then :; fi
f() { false; echo notreached; }
true && false && echo no
echo "should not print"
