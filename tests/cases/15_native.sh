# native arithmetic / tests: edge cases
x=5; y=3
echo $((x+y)) $((x-y)) $((x*y)) $((x/y)) $((x%y)) $((x**y)) $((x<<2)) $((x>>1)) $((-x)) $((!x)) $((~x))
echo $((x>y)) $((x<y)) $((x>=5)) $((x<=4)) $((x==5)) $((x!=5)) $((x&y)) $((x|y)) $((x^y))
echo $((x>3 && y>3)) $((x>3 || y>3)) $((x>3 ? 10 : 20)) $((x<3 ? 10 : 20))
echo $((x+=2)) $x $((x-=1)) $((x*=2)) $((x/=3)) $((x%=3)) $((x++)) $x $((++x)) $((x--)) $((--x))
a[2]=7; echo $((a[2]+1)) $((a[2]=9)) ${a[2]}
echo $((0x10)) $((010)) $((2#101)) $((-7/2)) $((-7%3))
unset u; echo $((u+1)) $((u=4)) $u
s="12"; echo $((s+1))
n=10; (( n > 5 )) && echo big; (( n < 5 )) || echo notsmall; ((n+=1)); echo $n
for ((i=0;i<3;i++)); do echo -n "$i "; done; echo
i=0; while ((i<3)); do i=$((i+1)); done; echo $i
r=$((3+4)); echo $r
if [ $x -eq 3 ]; then echo eq3; else echo not3; fi
if [ "$s" = "12" ]; then echo s12; fi
if [ "$s" != "12" ]; then echo no; else echo yes; fi
if [ -z "" ]; then echo empty; fi
if [ -n "$s" ]; then echo nonempty; fi
if [ -f /etc/passwd ] && [ -d /tmp ]; then echo files; fi
if [ ! -e /nonexistent ]; then echo missing; fi
if [ $x -gt 1 -a $x -lt 100 ]; then echo range; fi
if [ $x -lt 1 -o $x -gt 1 ]; then echo either; fi
[ "$s" ] && echo str
t=abc; [[ $t == a* ]] && echo glob; [[ $t == "a*" ]] || echo noglob; [[ $t == abc ]] && echo lit
[[ $t =~ ^a.c$ ]] && echo re
[[ -n $t && $x -gt 1 ]] && echo both
[[ ! $t == x ]] && echo notx
[[ ( 1 -lt 2 ) && ( 3 -gt 2 ) ]] && echo paren
[ 5 -lt abc ] 2>/dev/null; echo "st=$?"
v=abc; [ $v -lt 3 ] 2>/dev/null; echo "st=$?"
until [ $n -le 8 ]; do n=$((n-1)); done; echo $n
set -- 4 5; [ $1 -lt $2 ] && echo posl; echo $(( $1 + $2 ))
f() { [ $# -eq 2 ] && echo two; }; f a b
q=""; [ "$q" = "" ] && echo qe
