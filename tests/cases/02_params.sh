v="hello world"; u=
echo "${v:-d} ${u:-d} ${w:-d} ${u-d}"
echo "${v:+alt} ${u:+alt}|"
echo "${w:=assigned} $w"
echo "${#v} ${v:3} ${v:3:4} ${v: -3}"
echo "${v#hello } ${v##*o} ${v%world} ${v%%o*}"
echo "${v/o/0} ${v//o/0} ${v/#hello/HI} ${v/%world/W}"
echo "${v^^} ${v^} ${v,,}"
f=/usr/local/lib/file.tar.gz
echo "${f##*/} ${f%/*} ${f%%.*} ${f#*.}"
x=abc; ref=x; echo "${!ref}"
echo "${nothing:-$(echo sub)}"
echo ${v:-"a b c"}
