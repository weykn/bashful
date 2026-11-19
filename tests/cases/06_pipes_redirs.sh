cd "$(mktemp -d)"
echo hello > f1; echo world >> f1; cat f1
cat < f1 | tr a-z A-Z
echo err >&2 2>/dev/null
{ echo out; echo err >&2; } > both 2>&1; cat both
printf 'b\na\nc\na\n' | sort | uniq -c
printf '1\n2\n3\n' | while read n; do echo "got $n"; done
while read -r line; do echo "L:$line"; done < f1
cat <<EOF2
heredoc $((1+1))
EOF2
cat <<'EOF2'
raw $HOME
EOF2
cat <<< "here string"
grep -c o f1; echo "grep rc $?"
grep -q zzz f1; echo "grep rc $?"
ls | head -2
echo x | tee t1 >/dev/null; cat t1
wc -l < f1
exec 3>fd3; echo via3 >&3; exec 3>&-; cat fd3
false | true; echo "pipe rc $?"
set -o pipefail; false | true; echo "pipefail rc $?"
