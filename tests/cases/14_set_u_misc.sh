set -u
x=1; echo $x
echo "${undefined:-ok}"
echo ${undefined}
echo notreached
