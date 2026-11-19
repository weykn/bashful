x=1
( x=2; echo "in $x"; cd /; pwd )
echo "out $x"; pwd >/dev/null
( exit 3 ); echo "rc=$?"
y=$(echo a; echo b); echo "$y"
z=`echo back`; echo $z
echo "$(echo "nested $(echo deep)")"
echo $(echo  a   b   c)
echo "$(printf 'x\n\n\n')end"
r=$(false); echo "r=$? [$r]"
n=$(echo 1 2 3 | wc -w); echo $n
{ x=3; }; echo $x
cat <(echo procsub)
