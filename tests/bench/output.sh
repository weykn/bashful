# lots of output: echo / printf
for ((i = 0; i < 40000; i++)); do echo "row $i"; done | tail -1
for ((i = 0; i < 20000; i++)); do printf '%-6d|%5s|%x\n' "$i" "v$i" "$i"; done | tail -1
{ for ((i = 0; i < 20000; i++)); do echo "$i"; done; } > /dev/null; echo done
