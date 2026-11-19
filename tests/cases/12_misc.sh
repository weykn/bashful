echo "a  b" | { read x y; echo "$y|$x"; }
IFS=: read a b c <<< "1:2:3"; echo "$c$b$a"
x=hello; echo "${x}world" "$x"world
echo 'single $x' "double $x" $'tab\there'
echo "tilde: ~" ~ >/dev/null; echo ~/x | grep -c /x
FOO=bar; export FOO; env | grep -c '^FOO=bar$'
V=1 env | grep -c '^V=1$'; echo "V:${V-unset}"
readonly RO=1; echo $RO
unset FOO; echo "[${FOO-unset}]"
echo a;echo b&&echo c||echo d
[ -n "$RANDOM" ] && echo rnd
echo $((RANDOM%1))
i=0; while :; do i=$((i+1)); [ $i -ge 3 ] && break; done; echo "i=$i"
echo "$@" "$#"; echo "x${@}y"
f() { echo "$FUNCNAME" >/dev/null; return; }; f; echo "rc=$?"
e() { return 7; }; e || echo "caught $?"
exit 5
