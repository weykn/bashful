# real programs, forked pipelines, background jobs
printf 'b\na\nc\n' | /usr/bin/sort
echo hello | sed 's/h/J/'
x=$(echo abc | awk '{print toupper($0)}'); echo "$x"
yes | head -3
seq 1 5 | awk '{s+=$1} END {print s}'
nosuchcommand_xyz 2>/dev/null; echo "rc=$?"
sleep 0.2 & pid=$!; [ -n "$pid" ] && echo "bg ok"; wait $pid; echo "wait=$?"
(exit 3) & wait $!; echo "bgexit=$?"
f() { echo "in f $1"; }; f x
ls / | grep -c '^etc$'
echo one two | tr a-z A-Z | rev
env FOO=bar sh -c 'echo $FOO'
FOO=baz sh -c 'echo $FOO'
export E1=exported; sh -c 'echo $E1'
command -v sh >/dev/null && echo has-sh
cat <<END | wc -l
1
2
END
true | false; echo "pipe=$?"
set -o pipefail; false | true; echo "pf=$?"; set +o pipefail
while read -r l; do echo "got $l"; done < <(printf 'x\ny\n')
grep -o 'b.' <<< "abcbd"
sort -k2 -t, <<< $'a,2\nb,1'
date -d '2020-01-01' +%Y 2>/dev/null || echo nodate
