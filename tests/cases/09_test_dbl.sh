touch /tmp/_b2l_f; [ -f /tmp/_b2l_f ] && echo file; [ -d /tmp ] && echo dir; [ -e /nonexistent ] || echo noexist
[ -z "" ] && echo empty; [ -n "x" ] && echo nonempty
[ 1 -lt 2 -a 2 -lt 3 ] && echo and; [ 1 -gt 2 -o 3 -gt 2 ] && echo or
[ "a" != "b" ] && echo ne; [ ! -f /nope ] && echo notfile
[[ foo == f* ]] && echo glob; [[ foo == "f*" ]] || echo quoted
[[ abc =~ ^a.c$ ]] && echo regex; [[ -n $HOME && 1 -lt 2 ]] && echo dbl
x=""; [[ -z $x ]] && echo emptyvar; [[ $x == "" ]] && echo eq
test 5 -ge 5 && echo ge; [[ a < b ]] && echo lt
[[ ( 1 -eq 1 || 1 -eq 2 ) && ! 1 -eq 3 ]] && echo grp
rm -f /tmp/_b2l_f
