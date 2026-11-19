#!/bin/bash
# differential tests: compiled binary vs real bash (stdout + exit status)
cd "$(dirname "$0")"; ROOT=..; OUT=out; mkdir -p $OUT
export LC_ALL=C TZ=UTC
pass=0; fail=0
for t in cases/*.sh; do
  n=$(basename $t .sh)
  strict=--strict; case $n in *fallback*) strict=;; esac    # everything else must compile natively
  if ! $ROOT/bashful $strict $t -o $OUT/$n.bin 2>$OUT/$n.cerr; then echo "FAIL $n (compile)"; head -3 $OUT/$n.cerr; fail=$((fail+1)); continue; fi
  (cd /tmp && bash $OLDPWD/$t > $OLDPWD/$OUT/$n.exp 2>/dev/null </dev/null; echo "rc=$?" >> $OLDPWD/$OUT/$n.exp)
  (cd /tmp && $OLDPWD/$OUT/$n.bin > $OLDPWD/$OUT/$n.got 2>$OLDPWD/$OUT/$n.err </dev/null; echo "rc=$?" >> $OLDPWD/$OUT/$n.got)
  if diff -q $OUT/$n.exp $OUT/$n.got >/dev/null; then echo "ok   $n"; pass=$((pass+1)); else echo "FAIL $n"; diff $OUT/$n.exp $OUT/$n.got | head -${DIFFLINES:-8}; fail=$((fail+1)); fi
done
echo "passed $pass, failed $fail"; [ $fail = 0 ]
