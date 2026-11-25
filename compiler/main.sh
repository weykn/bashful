# main.sh - command line driver, runtime build cache, clang invocation
# RUNTIME_DIR and OPT are set by the build (see the Makefile).

usage() {
	printf '%s\n' "usage: bashful [options] script.sh [args...]   compile and run the script" \
		"       bashful [options] script.sh -o FILE     compile to an executable" \
		"       bashful [-i] [args...]                  interactive shell (like bash -i)" \
		"  -o, --output FILE   output file" \
		"  -r, --run           compile and run (the default without -o or --emit-*)" \
		"  -c, --command STR   compile and run STR" \
		"  -i, --interactive   interactive shell: every command is compiled and run natively; reads" \
		"                      /etc/bash.bashrc, ~/.bashrc and ~/.bashfulrc like bash -i reads its startup files" \
		"  --norc              interactive shell without the startup files" \
		"  --emit-ir           write LLVM IR" \
		"  --emit-bc           write LLVM bitcode" \
		"  --strict            fail instead of falling back to embedded bash for unsupported features" \
		"  --no-embed          run external commands (cat, grep, sort, ...) with the programs installed on the" \
		"                      system instead of the copies built into the executable" \
		"  -q, --quiet         no fallback note" \
		"  -h, --help          show this help"
}
slurp() {   # path -> SRC
	SRC=
	if [[ $1 == - ]]; then IFS= read -r -d '' SRC; return 0; fi
	[[ -r $1 && ! -d $1 ]] || die "cannot open $1"
	IFS= read -r -d '' SRC < "$1"
	return 0
}
# clang every runtime .c file in parallel into "<outdir>/<name>.<ext>" -> RTOBJS
rt_compile_all() {   # flags outdir ext
	local f o b p rc=0
	local -a pids
	RTOBJS=()
	for f in "$RUNTIME_DIR"/*.c; do
		b=${f##*/}
		o=$2/${b%.c}.$3
		clang -c $1 -w -I"$RUNTIME_DIR" "$f" -o "$o" &
		pids+=($!)
		RTOBJS+=("$o")
	done
	for p in "${pids[@]}"; do wait $p || rc=1; done
	return $rc
}
# Prebuilt runtime, cached under $XDG_CACHE_HOME/bashful: one native object (rt.o) plus an import library
# (rt_inl.ll + its index imp/) of its small functions, see rtlink.sh.  A compile then optimizes only the script
# with the runtime helpers it calls inlined, and does a plain link.
#   rt-<hash of the sources>/   the build; src = a copy of the sources it was built from
#   rt-<hash>/w/                witnesses: copies of the sources with the same modification times (cp -p)
#   last-<hash of the dir>      which build this RUNTIME_DIR used last
# A compile normally only checks that no source's modification time differs from its witness (like the C
# version's mtime key, to the nanosecond); otherwise the sources are read and compared in full.
# -> RT_DIR RT_OBJ RT_IMP
rt_cached() {
	local f x content base dir stamp tmpdir list= ptr wtmp ok
	if [[ -n $XDG_CACHE_HOME ]]; then base=$XDG_CACHE_HOME/bashful
	elif [[ -n $HOME ]]; then base=$HOME/.cache/bashful
	else return 1; fi
	for f in "$RUNTIME_DIR"/*; do
		case $f in *.c | *.h | *.def) list+=" ${f##*/}" ;; esac
	done
	shash "$RUNTIME_DIR"$'\1'"$OPT"$'\1'import-sh4$'\1'"$list"
	ptr=$base/last-$R
	if [[ -r $ptr ]] && IFS= read -r dir < "$ptr" && [[ -r $dir/ok && -r $dir/w/list ]] && IFS= read -r x < "$dir/w/list" && [[ $x == "$list" ]]; then
		ok=1
		for f in $list; do
			if [[ ! -e $dir/w/$f || $RUNTIME_DIR/$f -nt $dir/w/$f || $RUNTIME_DIR/$f -ot $dir/w/$f ]]; then ok=0; break; fi
		done
		if ((ok)); then RT_DIR=$dir RT_OBJ=$dir/rt.o RT_IMP=$dir/imp; return 0; fi
	fi
	# slow path: witness the sources first, then read them from the witnesses (so the witnessed times always
	# belong to the content that is checked)
	mkdir -p "$base" || return 1
	wtmp=$base/w.$$
	rm -rf "$wtmp"
	mkdir "$wtmp" || return 1
	for f in $list; do cp -p "$RUNTIME_DIR/$f" "$wtmp/" || { rm -rf "$wtmp"; return 1; }; done
	printf '%s\n' "$list" > "$wtmp/list"
	content=$RUNTIME_DIR$'\1'$OPT$'\1'import-sh4
	for f in $list; do
		x=
		IFS= read -r -d '' x < "$wtmp/$f"
		content+=$'\1'$f$'\1'$x
	done
	shash "$content"
	dir=$base/rt-$R stamp=$base/rt-$R/ok
	ok=0
	if [[ -r $stamp && -r $dir/src ]]; then
		x=
		IFS= read -r -d '' x < "$dir/src"
		[[ $x == "$content" ]] && ok=1
	fi
	if ((!ok)); then   # build into a private directory, then move into place (concurrent compiles stay safe)
		mkdir -p "$dir" || { rm -rf "$wtmp"; return 1; }
		tmpdir=$dir/build.$$
		mkdir -p "$tmpdir" || { rm -rf "$wtmp"; return 1; }
		if ! rt_compile_all "-emit-llvm $OPT" "$tmpdir" bc || ! llvm-link -S "${RTOBJS[@]}" -o "$tmpdir/rt.ll" ||
			! rtl_build "$tmpdir/rt.ll" "$tmpdir/rt_prom.ll" "$tmpdir/rt_inl.ll" "$tmpdir/imp" ||
			! clang -c $OPT -w -x ir -Xclang -disable-llvm-optzns "$tmpdir/rt_prom.ll" -o "$tmpdir/rt.o"; then
			rm -rf "$tmpdir" "$wtmp"
			return 1
		fi
		printf '%s' "$content" > "$tmpdir/src" || { rm -rf "$tmpdir" "$wtmp"; return 1; }
		rm -f "$stamp"
		rm -rf "$dir/imp" "$dir/bin"   # executables linked with a runtime that is being replaced
		mv -f "$tmpdir/rt.o" "$tmpdir/rt_inl.ll" "$tmpdir/imp" "$tmpdir/src" "$dir/" || { rm -rf "$tmpdir" "$wtmp"; return 1; }
		rm -rf "$tmpdir"
		touch "$stamp" || { rm -rf "$wtmp"; return 1; }
		find "$base" -maxdepth 1 -name 'rt-*' ! -name "${dir##*/}" -mmin +1440 -exec rm -rf {} + 2> /dev/null   # old runtimes
	fi
	rm -rf "$dir/w"
	mv "$wtmp" "$dir/w" 2> /dev/null || rm -rf "$wtmp"
	printf '%s\n' "$dir" > "$ptr.$$" && mv -f "$ptr.$$" "$ptr"
	RT_DIR=$dir RT_OBJ=$dir/rt.o RT_IMP=$dir/imp
	return 0
}
copy_file() {   # src dst: copy as an executable; status 1 if src does not exist or the copy failed (dst removed)
	[[ -f $1 ]] || return 1
	rm -f "$2"
	cp "$1" "$2" && return 0
	rm -f "$2"
	return 1
}
# finished executables are cached too, keyed on everything that goes into the link: an unchanged script is a
# file copy instead of a compile.  KEY holds the cache entry, KEY.key the exact link inputs it was built from.
bin_cache_get() {   # keytext final
	local x
	[[ -f $KEY && -f $KEY.key ]] || return 1
	x=
	IFS= read -r -d '' x < "$KEY.key"
	[[ $x == "$1" ]] || return 1
	copy_file "$KEY" "$2" || return 1
	touch "$KEY"
}
bin_cache_put() {   # keytext final
	local d=${KEY%/*} tmp=$KEY.$$
	mkdir -p "$d"
	if copy_file "$2" "$tmp"; then
		printf '%s' "$1" > "$tmp.key" && mv -f "$tmp.key" "$KEY.key" && mv -f "$tmp" "$KEY" || rm -f "$tmp" "$tmp.key"
	fi
	if ((($$ & 63) == 0)); then find "$d" -type f -mtime +7 -delete 2> /dev/null; fi   # now and then: drop entries unused for a week
	return 0
}
have_lld() {   # ld.lld somewhere in PATH
	local p=$PATH: d
	while [[ -n $p ]]; do
		d=${p%%:*}
		p=${p#*:}
		[[ -n $d && -x $d/ld.lld ]] && return 0
	done
	return 1
}
# compile SRC (named $1 in messages) into $2: an executable (MODE 0), LLVM IR (1) or bitcode (2); "-" with
# MODE 1 leaves the IR in TMP_OUT for the caller to print.  BF_EPILOGUE/BF_PROLOGUE: bash code put around SRC
# when it has to run on the bash fallback (interactive mode).
build_exe() {   # srcname final
	local srcname=$1 final=$2 rc tmp_ll tmp_dir dst lld src_ll x keytext merged=0 objs=
	tmp_ll=/tmp/bashful.$$.ll
	rm -f "$tmp_ll.unsup"
	(compile_native "$srcname" "$tmp_ll" "$tmp_ll.unsup")
	rc=$?
	if ((rc == 3)); then
		UNSUP=
		[[ -r $tmp_ll.unsup ]] && IFS= read -r -d '' UNSUP < "$tmp_ll.unsup"
		rm -f "$tmp_ll.unsup"
		((NOFALLBACK)) && { rm -f "$tmp_ll"; die "$srcname: cannot compile natively: $UNSUP"; }
		((QUIET)) || printf 'bashful: note: %s: %s -> using the embedded-bash fallback for this script (exact bash semantics, no native speedup)\n' "$srcname" "$UNSUP" >&2
		compile_fallback "$BF_PROLOGUE$SRC$BF_EPILOGUE" "$srcname"
		printf '%s' "$R" > "$tmp_ll" || die "cannot write $tmp_ll"
	elif ((rc)); then
		rm -f "$tmp_ll" "$tmp_ll.unsup"
		exit $rc
	fi
	tmp_dir=/tmp/bashful.$$.rt
	if ((MODE == 1)); then TMP_OUT=/tmp/bashful.$$.out.ll; else TMP_OUT=/tmp/bashful.$$.out.bc; fi
	((MODE == 0)) && rt_cached && objs=$RT_OBJ
	if ((MODE != 0)) && [[ $final != - ]]; then dst=$final; else dst=$TMP_OUT; fi
	if [[ -n $objs ]]; then   # fast path: optimize the script (+ inlinable runtime helpers), link the cached native runtime
		lld=
		have_lld && lld=-fuse-ld=lld
		# the executable cache is per runtime, so the script's own IR is a complete key: what the importer adds
		# depends on nothing else, and a cache hit skips it
		x=
		IFS= read -r -d '' x < "$tmp_ll"
		keytext=$x$'\1'$objs$'\1'$lld$'\1'$OPT
		shash "$keytext"
		KEY=$RT_DIR/bin/$R
		if bin_cache_get "$keytext" "$final"; then
			rm -f "$tmp_ll"
		else
			src_ll=$tmp_ll
			rtl_import "$tmp_ll" "$RT_IMP" "$tmp_ll.m" && merged=1 src_ll=$tmp_ll.m
			if ((merged)); then
				clang $lld $OPT -w -x ir "$src_ll" -x none "$objs" -o "$final" 2> /dev/null
				rc=$?
				if ((rc)); then   # should not happen; never let the importer break a build: retry without it
					clang $lld $OPT -w -x ir "$tmp_ll" -x none "$objs" -o "$final"
					rc=$?
				fi
			else
				clang $lld $OPT -w -x ir "$tmp_ll" -x none "$objs" -o "$final"
				rc=$?
			fi
			((rc)) || bin_cache_put "$keytext" "$final"
			rm -f "$tmp_ll" "$tmp_ll.m"
			((rc)) && die "clang failed to build the executable"
		fi
	else   # no cache: whole-program bitcode link (also the path for --emit-ir / --emit-bc)
		if ! mkdir -p "$tmp_dir" || ! rt_compile_all "-emit-llvm -O2" "$tmp_dir" bc; then
			rm -rf "$tmp_dir" "$tmp_ll"
			die "could not compile the embedded runtime (need clang in PATH)"
		fi
		if ((MODE == 1)); then llvm-link -S "$tmp_ll" "${RTOBJS[@]}" -o "$dst"; else llvm-link "$tmp_ll" "${RTOBJS[@]}" -o "$dst"; fi
		rc=$?
		rm -rf "$tmp_ll" "$tmp_dir"
		((rc)) && die "could not link the embedded runtime (need llvm-link in PATH)"
		if ((MODE == 0)); then
			clang -O2 -w "$TMP_OUT" -o "$final"
			rc=$?
			rm -f "$TMP_OUT"
			((rc)) && die "clang failed to build the executable"
		fi
	fi
	return 0
}
main() {
	local in= out= runflag=0 hascmd=0 cmdtext= srcname a rc final b keep path i interactive=0
	local -a sargs args
	MODE=0 NOFALLBACK=0 QUIET=0
	load_tools
	while (($#)); do
		a=$1
		if ((interactive)) || { [[ -n $in ]] && ((runflag || hascmd)); }; then sargs+=("$a"); shift; continue; fi   # everything after the script goes to the script
		case $a in
		-o | --output)
			(($# > 1)) || { usage >&2; die "unknown option $a"; }
			out=$2
			shift
			;;
		-c | --command | --inline)
			(($# > 1)) || { usage >&2; die "unknown option $a"; }
			cmdtext=$2 hascmd=1 in=bash CMD_MODE=1
			shift
			;;
		-i | --interactive) interactive=1 REPL_RC=1 ;;
		--norc) REPL_NORC=1 ;;
		-r | --run) runflag=1 ;;
		--emit-ir) MODE=1 ;;
		--emit-bc) MODE=2 ;;
		--no-fallback | --strict) NOFALLBACK=1 ;;
		-q | --quiet) QUIET=1 ;;
		--no-embed) NO_EMBED=1 ;;
		-h | --help) usage; exit 0 ;;
		-?*)
			# after the script, anything that is not a compiler option starts the script's arguments
			if [[ -n $in && -z $out ]] && ((MODE == 0)); then runflag=1; sargs+=("$a")
			else usage >&2; die "unknown option $a"; fi
			;;
		*)
			if [[ -z $in ]]; then in=$a
			elif [[ -z $out ]] && ((MODE == 0)); then runflag=1; sargs+=("$a")   # bashful script.sh args...
			else die "unexpected argument $a"; fi
			;;
		esac
		shift
	done
	if ((interactive)); then
		[[ -z $in && -z $out ]] && ((!hascmd && !runflag && MODE == 0)) || die "-i/--interactive takes no script, -c, -r, -o or --emit-*: arguments after it become \$1, \$2, ..."
		repl "${sargs[@]}"
	fi
	# no script at all: an interactive shell, like bash (only the options a shell takes)
	if [[ -z $in && -z $out ]] && ((!hascmd && !runflag && MODE == 0 && !NOFALLBACK)); then repl; fi
	if [[ -z $in ]]; then usage >&2; exit 1; fi
	((hascmd && MODE == 0)) && runflag=1
	if ((hascmd && ${#sargs[@]} > 0)); then   # like bash -c: the first extra argument is $0
		in=${sargs[0]}
		args=()
		for ((i = 1; i < ${#sargs[@]}; i++)); do args+=("${sargs[i]}"); done
		sargs=("${args[@]}")
	fi
	((runflag && MODE != 0)) && die "--run cannot be combined with --emit-ir/--emit-bc"
	((MODE == 0 && !runflag)) && [[ -z $out ]] && runflag=1   # bashful script.sh: compile and run it
	[[ $out == - ]] && ((MODE != 1)) && die "only --emit-ir can write to stdout"
	if ((hascmd)); then SRC=$cmdtext srcname=-c; else slurp "$in"; srcname=$in; fi

	# default output name: input basename without extension (never overwrite the input)
	final=$out
	[[ -z $final ]] && ((runflag)) && final=/tmp/bashful.$$.run
	if [[ -z $final ]]; then
		b=${in##*/}
		[[ $in == - ]] && b=a
		if ((MODE == 0)); then
			if [[ $b == ?*.* ]]; then final=${b%.*}; else final=a.out; fi
		else
			[[ $b == ?*.* ]] && b=${b%.*}
			if ((MODE == 2)); then final=$b.bc; else final=$b.ll; fi
		fi
	fi
	build_exe "$srcname" "$final"
	if ((runflag)); then
		keep=0
		[[ -n $out ]] && keep=1
		path=$final
		((keep)) && [[ $path != */* ]] && path=./$path
		(exec -a "$in" "$path" "${sargs[@]}")
		rc=$?
		((keep)) || rm -f "$final"
		exit $rc
	elif [[ $final == - ]]; then
		cat "$TMP_OUT"
		rm -f "$TMP_OUT"
	fi
	exit 0
}
