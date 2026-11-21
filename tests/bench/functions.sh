# function calls: recursion, locals, return values via REPLY and $(...)
fib() { if (($1 < 2)); then REPLY=$1; return; fi; local a; fib $(($1 - 1)); a=$REPLY; fib $(($1 - 2)); REPLY=$((a + REPLY)); }
fib 21; echo "fib 21 = $REPLY"
fact() { if (($1 <= 1)); then echo 1; else echo $(($1 * $(fact $(($1 - 1))))); fi; }
t=0
for ((i = 0; i < 300; i++)); do v=$(fact 12); ((t += v % 1000)); done
echo "fact: $v $t"
acc() { local x=$1 y=${2:-0}; total=$((total + x * y)); }
total=0
for ((i = 0; i < 30000; i++)); do acc $i 3; done
echo "total: $total"
