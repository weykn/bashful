n=3
if [ $n -eq 3 ]; then echo three; elif [ $n -eq 4 ]; then echo four; else echo other; fi
if [ "$n" = 4 ]; then echo four; else echo notfour; fi
case "$n" in 1|2) echo low;; 3) echo three;; *) echo other;; esac
case "foo.txt" in *.txt) echo text;; *.sh) echo shell;; esac
case abc in a?c) echo match;; esac
until [ $n -eq 0 ]; do echo $n; n=$((n-1)); done
for i in 1 2 3 4 5; do [ $i -eq 2 ] && continue; [ $i -eq 4 ] && break; echo "i$i"; done
for i in 1 2; do for j in a b; do [ $j = b ] && break; echo "$i$j"; done; done
if true; then echo t; fi
if false; then echo t; fi; echo "rc=$?"
while false; do :; done; echo "w=$?"
