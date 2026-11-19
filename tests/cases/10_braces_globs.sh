echo {a,b,c} x{1,2}y {1..5} {a..e} {5..1} {01..03} {1..10..3} {a,b}{1,2}
cd "$(mktemp -d)"; touch a.txt b.txt c.log
echo *.txt; echo *.nope; echo "*.txt"; ls *.log
for f in *.txt; do echo "f=$f"; done
echo [ab].txt ?.log
mkdir -p d/e; touch d/e/z; echo d/*/z
