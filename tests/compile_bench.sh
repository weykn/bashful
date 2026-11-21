#!/usr/bin/env bash
# compile_bench.sh - time how long bashful takes to COMPILE scripts (script.sh -> executable).
# The produced binaries are never run.
#
#   tests/compile_bench.sh                    every script in tests/bench/, 5 runs each
#   tests/compile_bench.sh -n 10 tests/cases  any mix of scripts and directories (*.sh inside)
#
# Cache modes (bashful keeps a prebuilt runtime + a cache of finished executables in $XDG_CACHE_HOME/bashful):
#   --mode new   (default) runtime prebuilt, executable cache emptied before every run
#                = compiling a script you have not compiled before (the normal case)
#   --mode hot   nothing cleared: recompiling an unchanged script (executable cache hit)
#   --mode cold  everything cleared: very first compile, includes building the runtime
# The real cache is never touched; runs use a private XDG_CACHE_HOME that is deleted afterwards.
#
# Other flags: --strict (no fallback), --no-embed, --emit-ir / --emit-bc (time those outputs instead),
#              --bashful PATH
export LC_ALL=C TZ=UTC

usage() { echo "usage: compile_bench.sh [-n RUNS] [--mode new|hot|cold] [--strict] [--no-embed] [--emit-ir|--emit-bc] [--bashful PATH] [script.sh|dir ...]"; }

runs=5 mode=new bflags=() bashful="" targets=() ext=bin
while (($#)); do
	case $1 in
	-n) runs=$2; shift ;;
	--mode) mode=$2; shift ;;
	--strict) bflags+=(--strict) ;;
	--no-embed) bflags+=(--no-embed) ;;
	--emit-ir) bflags+=(--emit-ir); ext=ll ;;
	--emit-bc) bflags+=(--emit-bc); ext=bc ;;
	--bashful) bashful=$2; shift ;;
	-h | --help) usage; exit 0 ;;
	-*) usage >&2; exit 2 ;;
	*) targets+=("$1") ;;
	esac
	shift
done
[[ $runs =~ ^[1-9][0-9]*$ ]] || { echo "compile_bench: -n needs a positive number" >&2; exit 2; }
case $mode in new | hot | cold) ;; *) echo "compile_bench: --mode must be new, hot or cold" >&2; exit 2 ;; esac

here=$(dirname "$0")
case $here in /*) ;; *) here=$PWD/$here ;; esac
[[ -z $bashful ]] && bashful=$here/../bashful
case $bashful in /*) ;; *) bashful=$PWD/$bashful ;; esac
[[ -x $bashful ]] || { echo "compile_bench: no bashful compiler at $bashful (run make, or pass --bashful PATH)" >&2; exit 2; }
((${#targets[@]})) || targets=("$here/bench")

scripts=()
for t in "${targets[@]}"; do
	if [[ -d $t ]]; then
		for f in "$t"/*.sh; do [[ -e $f ]] && scripts+=("$f"); done
	elif [[ -e $t ]]; then
		scripts+=("$t")
	else
		echo "compile_bench: $t: no such file or directory" >&2; exit 2
	fi
done

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export XDG_CACHE_HOME=$work/cache

now() { REPLY=${EPOCHREALTIME/./}; }                       # microseconds
ms() { REPLY="$(($1 / 1000)).$(($1 % 1000 / 100))"; }       # us -> "12.3"

# reset the private cache according to the mode, before one timed compile
reset_cache() {
	case $mode in
	cold) rm -rf "$XDG_CACHE_HOME" ;;
	new) rm -rf "$XDG_CACHE_HOME"/bashful/rt-*/bin "$XDG_CACHE_HOME"/bashful/rt-*/thinlto 2> /dev/null ;;
	esac
}

# warm-up: build the prebuilt runtime once so "new"/"hot" do not pay for it in the first run
if [[ $mode != cold ]]; then
	printf 'echo warmup\n' > "$work/warmup.sh"
	"$bashful" -q "$work/warmup.sh" -o "$work/warmup.bin" > /dev/null 2>&1
fi

printf '%-18s %6s %10s %10s %10s %10s  %s\n' script lines min median mean max result
tot_min=0 tot_med=0 pass=0 fail=0
for s in "${scripts[@]}"; do
	n=$(basename "$s" .sh)
	lines=$(wc -l < "$s")
	out=$work/$n.$ext
	times=() ok=1 note=""
	# hot mode: one untimed compile first so the cache holds this script
	[[ $mode == hot ]] && "$bashful" -q "${bflags[@]}" "$s" -o "$out" > /dev/null 2>&1
	for ((r = 0; r < runs; r++)); do
		reset_cache
		rm -f "$out"
		now; t0=$REPLY
		"$bashful" "${bflags[@]}" "$s" -o "$out" > /dev/null 2> "$work/$n.err"
		rc=$?
		now; times+=($((REPLY - t0)))
		if ((rc)); then ok=0; break; fi
	done
	if ((!ok)); then
		printf '%-18s %6s %10s %10s %10s %10s  %s\n' "$n" "$lines" - - - - "FAIL (compile)"
		sed 's/^/    /' "$work/$n.err" | head -3
		((fail++)); continue
	fi
	grep -q 'embedded-bash fallback' "$work/$n.err" && note=" (fallback)"
	# sort ascending -> min, median, max; mean
	sorted=($(printf '%s\n' "${times[@]}" | sort -n))
	cnt=${#sorted[@]} sum=0
	for t in "${sorted[@]}"; do ((sum += t)); done
	mn=${sorted[0]} mx=${sorted[cnt - 1]}
	if ((cnt % 2)); then md=${sorted[cnt / 2]}; else md=$(((sorted[cnt / 2 - 1] + sorted[cnt / 2]) / 2)); fi
	mean=$((sum / cnt))
	((tot_min += mn, tot_med += md, pass++))
	ms $mn; a="$REPLY ms"; ms $md; b="$REPLY ms"; ms $mean; c="$REPLY ms"; ms $mx; d="$REPLY ms"
	printf '%-18s %6s %10s %10s %10s %10s  %s\n' "$n" "$lines" "$a" "$b" "$c" "$d" "ok$note"
done
ms $tot_min; a="$REPLY ms"; ms $tot_med; b="$REPLY ms"
printf '%-18s %6s %10s %10s\n' total "" "$a" "$b"
echo "mode $mode, $runs runs each; compiled $pass, failed $fail"
((fail == 0))
