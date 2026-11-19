cd "$(mktemp -d)"
printf 'line1\nline2\nline3\nline4\n' > t
head -n 2 t; tail -n 2 t; head -1 t; tail -n +3 t
wc t; wc -l t; wc -c < t
basename /a/b/c.txt .txt; dirname /a/b/c.txt; basename /a/b/
seq 3; seq 2 4; seq 1 2 7
echo "a,b,c" | cut -d, -f2; echo abcdef | cut -c2-4
echo hello | tr 'a-y' 'b-z'; echo hello | tr -d l; echo aabbcc | tr -s 'a-c'
printf '3\n1\n2\n' | sort -n; printf 'b\na\n' | sort -r; printf 'a\na\nb\n' | sort -u
cp t t2; mv t2 t3; ls; rm t3; ls
mkdir -p x/y/z; ls x; rmdir x/y/z; ls x/y
echo "$(date +%Y | wc -c)"
env FOO=bar sh -c true 2>/dev/null; echo done
printf '%5s|%-5s|%05d|%x|%c|%.2f\n' ab cd 42 255 z 3.14159
printf '%s\n' a b c
printf 'no newline'; echo
echo -e 'a\tb\\n'; echo -n x; echo; echo -e "\x41\0102"
printf '%b\n' 'a\nb'
grep -n line t | head -2; grep -v line2 t | wc -l; grep -i LINE1 t
echo abc | grep -E 'a(b|x)c'; echo a.c | grep -F a.c
touch -- q; test -e q && echo touched
kill -l | head -2
sleep 0.01; echo slept
