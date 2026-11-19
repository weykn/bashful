echo $((1+2*3)) $(( (1+2)*3 )) $((10/3)) $((10%3)) $((2**10)) $((-5+2))
x=5; echo $((x+1)) $((x*x)) $((x>3)) $((x==5 && 1)) $((x<<2))
((x++)); echo $x
((x+=10)); echo $x
y=$((x>10 ? 1 : 2)); echo $y
i=0; while [ $i -lt 3 ]; do echo "i=$i"; i=$((i+1)); done
for ((j=0; j<4; j++)); do echo -n "$j "; done; echo
(( x > 100 )) && echo big || echo small
echo $((0x1f)) $((010))
s=hello; echo $((${#s}+1)) $(( $(echo 4) * 2 )) $(($x + ${#s} * 2))
