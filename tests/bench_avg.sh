#!/usr/bin/env bash
# bench_avg.sh - run bench.sh several times and print the average time per benchmark instead of one run.
#
#   tests/bench_avg.sh                  10 runs of all benchmarks in tests/bench/
#   tests/bench_avg.sh -t 20 tests/cases 20 runs; every other argument is passed through to bench.sh
#
# Speedup is avg(bash) / avg(bashful).  A row is FAIL if it failed in any of the runs.
export LC_ALL=C

times=10 args=()
while (($#)); do
	case $1 in
	-t) times=$2; shift ;;
	-h | --help) echo "usage: bench_avg.sh [-t TIMES] [bench.sh args...]"; exit 0 ;;
	*) args+=("$1") ;;
	esac
	shift
done
[[ $times =~ ^[1-9][0-9]*$ ]] || { echo "bench_avg: -t needs a positive number" >&2; exit 2; }

here=$(dirname "$0")
out=$(mktemp)
trap 'rm -f "$out"' EXIT

for ((r = 1; r <= times; r++)); do
	[[ -t 2 ]] && printf "\rrun %d/%d" "$r" "$times" >&2
	"$here/bench.sh" "${args[@]}" >> "$out" 2>/dev/null
done
[[ -t 2 ]] && printf "\r%20s\r" "" >&2

# bench.sh rows: NAME  BASH ms  BIN ms  SPEEDUPx  RESULT...   (compile failures: NAME - - - FAIL (compile))
awk -v runs="$times" '
	$1 == "benchmark" || $1 == "total" || $1 == "passed" || /^    / { next }
	NF >= 2 {
		n = $1
		if (!(n in seen)) { seen[n] = 1; order[++cnt] = n }
		if ($2 == "-") { fail[n] = 1; next }
		bash[n] += $2; bin[n] += $4; got[n]++
		res = $7; for (i = 8; i <= NF; i++) res = res " " $i
		if (res ~ /^FAIL/) fail[n] = 1
		if (res ~ /fallback/) fb[n] = 1
	}
	function row(name, a, b, r) {
		printf "%-18s %11s %11s %9s  %s\n", name, sprintf("%.1f ms", a), sprintf("%.1f ms", b),
			sprintf("%.1fx", b > 0 ? a / b : 0), r
	}
	END {
		printf "%-18s %11s %11s %9s  %s\n", "benchmark", "avg bash", "avg bashful", "speedup", "result (" runs " runs)"
		for (i = 1; i <= cnt; i++) {
			n = order[i]
			if (!got[n]) { printf "%-18s %11s %11s %9s  %s\n", n, "-", "-", "-", "FAIL (compile)"; nfail++; continue }
			a = bash[n] / got[n]; b = bin[n] / got[n]; ta += a; tb += b
			r = fail[n] ? "FAIL" : "ok"; if (fb[n]) r = r " (fallback)"
			if (fail[n]) nfail++; else npass++
			row(n, a, b, r)
		}
		if (tb > 0) row("total", ta, tb, "")
		printf "passed %d, failed %d\n", npass, nfail
		exit nfail > 0
	}' "$out"
