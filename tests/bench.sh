#!/usr/bin/env bash
# bench.sh - compile each script with bashful, run it with bash and as a compiled binary, time both and
# check that the output and exit status are identical.
#
#   tests/bench.sh                     all benchmarks in tests/bench/
#   tests/bench.sh tests/cases         any mix of scripts and directories (*.sh inside)
#   tests/bench.sh -n 5 --no-embed     best of 5 runs; flags bashful does not know are rejected
#
# The runner is itself a bash script that bashful compiles natively, so it works both ways:
#   bash tests/bench.sh   or   ./bashful -r tests/bench.sh
#
# Inputs: a script NAME.sh is run once per stdin file NAME.in / NAME.*.in (or /dev/null if there are none),
# and, if NAME.args exists, once per line of it for each stdin (the line is word-split like shell code, so
# quotes work: "a b" c).  Each combination is its own result row, e.g. NAME[crlf] or NAME[empty]#2.
# Every run starts in a fresh empty directory.  Scripts that bashful cannot compile natively are marked "(fallback)".
export LC_ALL=C TZ=UTC

usage() {
	echo "usage: bench.sh [-n RUNS] [--no-embed] [--bashful PATH] [--bash PATH] [--keep] [script.sh|dir ...]"
}

runs=1 bflags="" keep=0 bash_bin=bash bashful="" targets=()
while (($#)); do
	case $1 in
	-n) runs=$2; shift ;;
	--no-embed) bflags="$bflags --no-embed" ;;
	--bashful) bashful=$2; shift ;;
	--bash) bash_bin=$2; shift ;;
	--keep) keep=1 ;;
	-h | --help) usage; exit 0 ;;
	-*) usage >&2; exit 2 ;;
	*) targets+=("$1") ;;
	esac
	shift
done

here=$(dirname "$0")
case $here in /*) ;; *) here=$PWD/$here ;; esac
[[ -z $bashful ]] && bashful=$here/../bashful
if [[ ! -x $bashful ]]; then echo "bench: no bashful compiler at $bashful (run make, or pass --bashful PATH)" >&2; exit 2; fi
((${#targets[@]})) || targets=("$here/bench")

scripts=()
for t in "${targets[@]}"; do
	if [[ -d $t ]]; then
		for f in "$t"/*.sh; do
			if [[ -e $f ]]; then scripts+=("$f")
			elif [[ -L $f ]]; then echo "bench: warning: $f is a broken symlink, skipped" >&2
			fi
		done
	elif [[ -e $t ]]; then
		scripts+=("$t")
	else
		echo "bench: $t: no such file or directory" >&2; exit 2
	fi
done

work=$(mktemp -d)

# microseconds since the epoch
now() {
	if [[ -n $EPOCHREALTIME ]]; then REPLY=${EPOCHREALTIME/./}; else REPLY=$(date +%s%N); REPLY=${REPLY%???}; fi
}
# ms with one decimal from microseconds
ms() { REPLY="$(($1 / 1000)).$(($1 % 1000 / 100))"; }

# time_run OUTFILE INFILE CMD...  -> REPLY = best wall time in microseconds over $runs runs
time_run() {
	local outf=$1 inf=$2 best=-1 r t0 dt rc
	shift 2
	for ((r = 0; r < runs; r++)); do
		rm -rf "$work/run"; mkdir "$work/run"
		now; t0=$REPLY
		(cd "$work/run" && "$@" < "$inf" > "$outf" 2> "$outf.err")
		rc=$?
		now; dt=$((REPLY - t0))
		((best < 0 || dt < best)) && best=$dt
	done
	echo "rc=$rc" >> "$outf"
	REPLY=$best
}

printf '%-18s %11s %11s %9s  %s\n' benchmark bash bashful speedup result
pass=0 fail=0 tot_bash=0 tot_bin=0
for s in "${scripts[@]}"; do
	case $s in /*) ;; *) s=$PWD/$s ;; esac
	n=$(basename "$s" .sh)
	base=${s%.sh}
	inputs=()
	[[ -e $base.in ]] && inputs+=("$base.in")
	for f in "$base".*.in; do [[ -e $f ]] && inputs+=("$f"); done
	((${#inputs[@]})) || inputs=(/dev/null)
	argsets=()
	if [[ -e $base.args ]]; then
		while IFS= read -r line || [[ -n $line ]]; do argsets+=("$line"); done < "$base.args"
	fi
	((${#argsets[@]})) || argsets=("")
	bin=$work/$n.bin note=""
	if ! "$bashful" $bflags --strict -q "$s" -o "$bin" 2> "$work/$n.cerr"; then
		if "$bashful" $bflags -q "$s" -o "$bin" 2>> "$work/$n.cerr"; then
			note=" (fallback)"
		else
			printf '%-18s %11s %11s %9s  %s\n' "$n" - - - "FAIL (compile)"
			sed 's/^/    /' "$work/$n.cerr" | head -5
			((fail++)); continue
		fi
	fi
	for inf in "${inputs[@]}"; do
		label=$n
		if [[ $inf != /dev/null && $inf != "$base.in" ]]; then
			v=${inf#"$base".}; label="$n[${v%.in}]"
		fi
		k=0
		for line in "${argsets[@]}"; do
			((k++))
			eval "args=($line)"
			lab=$label; ((${#argsets[@]} > 1)) && lab="$label#$k"
			time_run "$work/$n.exp" "$inf" "$bash_bin" "$s" "${args[@]}"; tb=$REPLY
			time_run "$work/$n.got" "$inf" "$bin" "${args[@]}"; tc=$REPLY
			((tot_bash += tb, tot_bin += tc))
			((tc > 0)) || tc=1
			x10=$((tb * 10 / tc))
			ms $tb; a="$REPLY ms"
			ms $tc; b="$REPLY ms"
			if cmp -s "$work/$n.exp" "$work/$n.got"; then
				result="ok$note"; ((pass++))
			else
				result="FAIL (output differs)$note"; ((fail++))
			fi
			printf '%-18s %11s %11s %7d.%dx  %s\n' "$lab" "$a" "$b" $((x10 / 10)) $((x10 % 10)) "$result"
			if [[ $result == FAIL* ]]; then
				diff "$work/$n.exp" "$work/$n.got" | head -"${DIFFLINES:-8}" | sed 's/^/    /'
			fi
		done
	done
done

if ((tot_bin > 0)); then
	x10=$((tot_bash * 10 / tot_bin))
	ms $tot_bash; a="$REPLY ms"
	ms $tot_bin; b="$REPLY ms"
	printf '%-18s %11s %11s %7d.%dx\n' total "$a" "$b" $((x10 / 10)) $((x10 % 10))
fi
echo "passed $pass, failed $fail"
if ((keep)); then echo "outputs kept in $work"; else rm -rf "$work"; fi
((fail == 0))
