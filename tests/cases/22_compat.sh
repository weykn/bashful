# comma operator, EPOCH*/SECONDS, arithmetic errors
a=1 b=2; ((a += 1, b += 2)); echo $a $b $((a, b * 10)) $(( (a = 5, a + 1) ))
for ((i = 0, j = 10; i < 3; i++, j--)); do echo -n "$i:$j "; done; echo
x=(1 2 3); echo $(( x[0, 2] ))
t=$EPOCHREALTIME; [[ $t =~ ^[0-9]+\.[0-9]{6}$ ]] && echo realtime-ok
(( EPOCHSECONDS > 1600000000 )) && echo seconds-ok
SECONDS=100; (( SECONDS >= 100 && SECONDS < 105 )) && echo SECONDS-ok
((1 +)) 2>/dev/null; echo "cmd status $?"
let "2 *" 2>/dev/null; echo "let status $?"
[[ 1+ -eq 1 ]] 2>/dev/null; echo "test status $?"
r=$(echo before; echo $((1 +)); echo not-here) 2>/dev/null; echo "[$r] $?"
(echo $((3 *)); echo not-here) 2>/dev/null; echo "sub $?"
echo $((4 /)) 2>/dev/null
echo not-reached
