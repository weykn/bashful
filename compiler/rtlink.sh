# rtlink.sh - runtime import
# Fast native builds without LTO, but with the runtime's small hot helpers still inlined into the script.
#
# Once per runtime (cached, see main.sh):  the runtime bitcode, linked into one module (rt.ll), becomes
#   rt_prom.ll  every internal/private symbol renamed to a unique hidden global (@__bfrt.NAME) so that code
#               copied into another module can still reach it; compiled once into the native rt.o
#   rt_inl.ll   an import library: the small functions as `available_externally` definitions (usable for
#               inlining, never emitted), everything else as declarations; attribute groups and metadata
#               renumbered (+RT_OFS) so they cannot collide with the script's.
#   imp/        an index of rt_inl.ll (per symbol: declaration, definition, what it refers to), so that a
#               compile reads only the symbols it imports and never re-scans the library
# Per compile (rtl_import):  the runtime functions the script references are copied into its IR, then the
#   functions those reference, with a size limit that shrinks with depth (like ThinLTO's importer);
#   everything else they need is declared.  One clang call then optimizes the script + imports and links rt.o.
RT_OFS=100000
MAXINSTR=64       # largest function rt_inl.ll offers for import
IMPORT_LIMIT=(64 24)   # max instructions by call depth from the script
declare -A LOC IDX SDECL SDEF
SPCH='["@#!]' STRAT='["@]' REFCH='["@;]' NUMCH='["#!]'

gname() {   # text right after an '@' -> R = "@name" (quoted names too), empty if none
	local t=$1 q
	if [[ $t == '"'* ]]; then
		q=${t:1}
		if [[ $q != *'"'* ]]; then R=; return 1; fi
		R="@\"${q%%'"'*}\""
		return 0
	fi
	t=${t%%$IDNOT*}
	if [[ -z $t ]]; then R=; return 1; fi
	R=@$t
}
# copy text, renaming local @names to @__bfrt.NAME and, if ofs, renumbering #N / !N by RT_OFS; string
# literals are copied verbatim
copy_ren() {   # text ofs -> R
	local s=$1 out= pre c rest q d nm pat=$STRAT
	(($2)) && pat=$SPCH
	if [[ $s != *$pat* ]]; then R=$s; return; fi
	while :; do
		pre=${s%%$pat*}
		out+=$pre
		[[ $pre == "$s" ]] && break
		s=${s:${#pre}}
		c=${s:0:1}
		case $c in
		'"')
			rest=${s:1}
			if [[ $rest != *'"'* ]]; then out+=$s; break; fi
			q=${rest%%'"'*}
			out+="\"$q\""
			s=${rest:${#q}+1}
			;;
		@)
			if gname "${s:1}"; then
				nm=$R
				if [[ -n ${LOC[$nm]} ]]; then
					if [[ $nm == '@"'* ]]; then out+="@\"__bfrt.${nm:2}"; else out+="@__bfrt.${nm:1}"; fi
				else out+=$nm; fi
				s=${s:${#nm}}
			else
				out+=@
				s=${s:1}
			fi
			;;
		*)   # # or !
			d=${s:1}
			d=${d%%[!0-9]*}
			if [[ -n $d && ${out: -1} != [a-zA-Z0-9\$._-] ]]; then
				out+="$c$((10#$d + RT_OFS))"
				s=${s:${#d}+1}
			else
				out+=$c
				s=${s:1}
			fi
			;;
		esac
	done
	R=$out
}
skip_linkage() {   # R = text without leading linkage / visibility / preemption keywords
	local p=$1 again=1 kw
	while ((again)); do
		again=0
		for kw in "internal " "private " "linkonce_odr " "linkonce " "weak_odr " "weak " "external " "common " \
			"extern_weak " "available_externally " "dso_local " "dso_preemptable " "hidden " "default " "protected "; do
			if [[ $p == "$kw"* ]]; then p=${p:${#kw}}; again=1; fi
		done
	done
	R=$p
}
skip_type() {   # text starting with an LLVM type -> R = the type
	local p=$1 i=0 d=0 c
	if [[ $p == [\[{\<]* ]]; then
		while :; do
			c=${p:i:1}
			case $c in '[' | '{' | '<' | '(') ((d++)) ;; ']' | '}' | '>' | ')') ((d--)) ;; esac
			((i++))
			((d > 0)) && [[ -n ${p:i:1} ]] || break
		done
		R=${p:0:i}
		return
	fi
	R=${p%%[ ,]*}
}
# rt.ll -> rt_prom.ll, rt_inl.ll, the import index directory
rtl_build() {   # rt_ll prom inl index_dir
	local n i j k l ln s r nm ninstr ok rest h g c kw kwpos ty al ae tl tle out ni kind first last rd e ix x ne
	local -a L prom inl
	LOC=()
	mapfile -t L < "$1" || return 1
	n=${#L[@]}
	for ((i = 0; i < n; i++)); do   # local definitions: define internal|private ..., @x = internal|private ...
		l=${L[i]}
		if [[ $l == "define internal "* || $l == "define private "* ]]; then
			gname "${l#*@}" && LOC[$R]=1
		elif [[ $l == @* ]] && gname "${l:1}"; then
			r=${l:${#R}}
			[[ $r == " = internal "* || $r == " = private "* ]] && LOC[$R]=1
		fi
	done
	for ((i = 0; i < n; i++)); do
		l=${L[i]} s=${L[i]} ln=
		if [[ $l == "define internal "* ]]; then ln="define hidden " s=${l:16}
		elif [[ $l == "define private "* ]]; then ln="define hidden " s=${l:15}
		elif [[ $l == @* ]] && gname "${l:1}" && [[ -n ${LOC[$R]} ]]; then
			nm=$R
			r=${l:${#nm}}
			copy_ren "$nm" 0
			ln=$R" = hidden "
			if [[ $r == " = internal "* ]]; then s=${r:12}; else s=${r:11}; fi
		fi
		copy_ren "$s" 0
		ln+=$R
		prom+=("$ln")
		if [[ $l == "define "* ]]; then   # the whole function: header .. "}"
			j=$((i + 1)) ninstr=0 ok=1
			while ((j < n)) && [[ ${L[j]} != '}' ]]; do
				[[ ${L[j]} == '  '* ]] && ((ninstr++))
				[[ ${L[j]} == *blockaddress* ]] && ok=0
				((j++))
			done
			skip_linkage "${ln:7}"
			rest=$R
			if ((ok && ninstr <= MAXINSTR)); then
				h=
				[[ $ln == "define hidden "* ]] && h="hidden "
				copy_ren "$rest" 1
				inl+=("define available_externally $h$R ;n=$ninstr")
				for ((k = i + 1; k <= j && k < n; k++)); do
					copy_ren "${L[k]}" 0
					prom+=("$R")
					copy_ren "${L[k]}" 1
					inl+=("$R")
				done
			else   # declaration: header without body, personality, comdat, prefix data
				h=$rest
				[[ $h == *'{'* ]] && h=${h%'{'*}
				while [[ $h == *' ' ]]; do h=${h% }; done
				copy_ren "$h" 1
				h=$R
				h=${h%%' personality '*}
				h=${h%%' comdat'*}
				inl+=("declare hidden $h")
				for ((k = i + 1; k <= j && k < n; k++)); do
					copy_ren "${L[k]}" 0
					prom+=("$R")
				done
			fi
			i=$j
			continue
		fi
		if [[ $ln == @* ]]; then   # global: declaration only
			gname "${ln:1}"
			nm=$R
			r=${ln:${#nm}}
			if [[ $r == " = external "* || $r == *" alias "* || $r == *" ifunc "* ]]; then inl+=("$ln")
			else
				g=${r%%' global '*} c=${r%%' constant '*}
				kw=
				if [[ $g != "$r" ]] && { [[ $c == "$r" ]] || ((${#g} < ${#c})); }; then kw=g
				elif [[ $c != "$r" ]]; then kw=c; fi
				if [[ -n $kw ]]; then
					if [[ $kw == g ]]; then ty=${r:${#g}+8}; else ty=${r:${#c}+10}; fi
					skip_type "$ty"
					ty=$R
					out="$nm = external hidden "
					tl=${r%%thread_local*}
					if [[ $kw == g ]]; then kwpos=${#g}; else kwpos=${#c}; fi
					if [[ $tl != "$r" ]] && ((${#tl} < kwpos)); then
						tle=${r:${#tl}}
						if [[ ${tle:12:1} == '(' ]]; then tle=${tle%%')'*}')'; else tle=${tle:0:12}; fi
						out+="$tle "
					fi
					if [[ $kw == g ]]; then out+="global $ty"; else out+="constant $ty"; fi
					if [[ $kw == g ]]; then al=${r:${#g}+8+${#ty}}; else al=${r:${#c}+10+${#ty}}; fi
					if [[ $al == *', align '* ]]; then
						al=${al#*', align '}
						ae=${al%%[!0-9]*}
						out+=", align $ae"
					fi
					inl+=("$out")
				fi
			fi
		elif [[ $l == "declare "* || $l == "attributes #"* || $l == %* || $l == '!'[0-9]* ]]; then
			copy_ren "$ln" 1
			inl+=("$R")
		fi
	done
	printf '%s\n' "${prom[@]}" > "$2" || return 1
	printf '%s\n' "${inl[@]}" > "$3" || return 1
	# The import index, so that a compile never re-scans rt_inl.ll: parallel files with one line per symbol
	#   ix_name, ix_kind (0 declare, 1 define, 2 global), ix_ni (instructions), ix_rd (the runtime defines it),
	#   ix_decl (its declaration), ix_dref / ix_bref (attribute group / metadata ids its declaration / its
	#   definition refers to), ix_nref (the @names its definition refers to; "!" = scan the text at import)
	# b/<symbol number>: the definition of each function; meta_ix / meta_line / meta_ref: attribute groups
	# (id 2N) and metadata nodes (id 2N+1) with the ids a metadata node refers to; types: type definitions.
	local d=$4 dl dec body nref bref
	local -a xn xk xni xrd xdecl xdref xbref xnref mix mline mref types
	mkdir -p "$d/b" || return 1
	n=${#inl[@]}
	ne=0
	for ((i = 0; i < n; i++)); do
		l=${inl[i]}
		kind=-1 ni=0 rd=0 e=
		if [[ $l == "declare "* ]]; then
			kind=0 e=${l#*@}
			[[ $l == "declare hidden "* ]] && rd=1
		elif [[ $l == "define "* ]]; then
			kind=1 e=${l#*@} rd=1 ni=1048576
			[[ $l == *' ;n='* ]] && { c_atoi "${l##*' ;n='}"; ni=$R; }
		elif [[ $l == @* ]]; then
			kind=2 e=${l:1}
			[[ $l == *' = external hidden '* ]] && rd=1
		elif [[ $l == "attributes #"* || $l == '!'[0-9]* ]]; then
			if [[ $l == '!'* ]]; then c_atoi "${l:1}"; ix=1; else c_atoi "${l:12}"; ix=0; fi
			if ((R >= RT_OFS && R - RT_OFS <= 1048576)); then
				mix+=($((2 * (R - RT_OFS) + ix)))
				mline+=("$l")
				REFX=
				if ((ix)); then x=${l:1}; x=${x%%[!0-9]*}; nums_of "${l:${#x}+1}"; fi
				mref+=("$REFX")
			fi
		elif [[ $l == %* ]]; then types+=("$l")
		fi
		((kind >= 0)) || continue
		gname "$e" || { [[ $kind == 1 ]] && while ((i < n)) && [[ ${inl[i]} != '}' ]]; do ((i++)); done; continue; }
		xn[ne]=$R xk[ne]=$kind xni[ne]=$ni xrd[ne]=$rd
		if ((kind == 1)); then
			dec=${l:28}
			[[ $dec == "hidden "* ]] && dec=${dec:7}
			dec="declare hidden ${dec%'{'*}"
			body= REFX= REFN= nref=
			while :; do
				body+=${inl[i]}$'\n'
				nums_of "${inl[i]}"
				names_of "${inl[i]}" || nref='!'
				[[ ${inl[i]} == '}' ]] || ((i + 1 >= n)) && break
				((i++))
			done
			printf '%s' "$body" > "$d/b/$ne" || return 1
			xbref[ne]=$REFX
			if [[ -n $nref ]]; then xnref[ne]=$nref; else xnref[ne]=$REFN; fi
		else
			dec=$l
			xbref[ne]= xnref[ne]=
		fi
		xdecl[ne]=$dec
		REFX=
		nums_of "$dec"
		xdref[ne]=$REFX
		((ne++))
	done
	printf '%s\n' "${xn[@]}" > "$d/ix_name" && printf '%s\n' "${xk[@]}" > "$d/ix_kind" &&
		printf '%s\n' "${xni[@]}" > "$d/ix_ni" && printf '%s\n' "${xrd[@]}" > "$d/ix_rd" &&
		printf '%s\n' "${xdecl[@]}" > "$d/ix_decl" && printf '%s\n' "${xdref[@]}" > "$d/ix_dref" &&
		printf '%s\n' "${xbref[@]}" > "$d/ix_bref" && printf '%s\n' "${xnref[@]}" > "$d/ix_nref" &&
		printf '%s\n' "${mix[@]}" > "$d/meta_ix" && printf '%s\n' "${mline[@]}" > "$d/meta_line" &&
		printf '%s\n' "${mref[@]}" > "$d/meta_ref" && printf '%s\n' "${types[@]}" > "$d/types" || return 1
	return 0
}
# REFX += " id" for every #N / !N (renumbered) outside string literals
nums_of() {
	local s=$1 pre c q rest dg v
	while :; do
		pre=${s%%$NUMCH*}
		[[ $pre == "$s" ]] && return 0
		s=${s:${#pre}}
		c=${s:0:1}
		if [[ $c == '"' ]]; then
			rest=${s:1}
			[[ $rest == *'"'* ]] || return 0
			q=${rest%%'"'*}
			s=${rest:${#q}+1}
			continue
		fi
		s=${s:1}
		dg=${s%%[!0-9]*}
		[[ -n $dg ]] || continue
		s=${s:${#dg}}
		v=$((10#$dg - RT_OFS))
		((v >= 0 && v < 1048576)) || continue
		if [[ $c == '!' ]]; then v=$((2 * v + 1)); else v=$((2 * v)); fi
		[[ " $REFX " == *" $v "* ]] || REFX+=" $v"
	done
}
# REFN += " @name" for every @name outside string literals and comments; status 1 if a name has a blank
names_of() {
	local s=$1 pre q rest
	[[ $s == *@* ]] || return 0
	while :; do
		pre=${s%%$REFCH*}
		[[ $pre == "$s" ]] && return 0
		s=${s:${#pre}}
		case ${s:0:1} in
		'"')
			rest=${s:1}
			[[ $rest == *'"'* ]] || return 0
			q=${rest%%'"'*}
			s=${rest:${#q}+1}
			;;
		';') return 0 ;;
		*)
			if gname "${s:1}"; then
				[[ $R == *[$' \t']* ]] && return 1
				[[ " $REFN " == *" $R "* ]] || REFN+=" $R"
				s=${s:${#R}}
			else s=${s:1}; fi
			;;
		esac
	done
}

# ---- per compile: script IR + needed imports ----
want() {   # @name referenced by imported or script code
	local e=${IDX[$1]}
	[[ -n $e ]] || return 0
	if [[ -n ${SDEF[$1]} ]]; then ((E_RD[e])) && CLASH=1; return 0; fi   # the script defines it itself
	((E_ST[e] == 2)) && return 0
	if ((E_KIND[e] == 1 && DEPTH < ${#IMPORT_LIMIT[@]} && E_NI[e] <= IMPORT_LIMIT[DEPTH])); then
		E_ST[e]=2
		IMPQ+=($e)
	elif ((!E_ST[e])); then E_ST[e]=1
	fi
	TOUCHED[e]=1
}
each_ref() {   # want() every @name on the line, skipping string literals and comments
	local s=$1 pre q rest
	[[ $s == *@* ]] || return 0
	while :; do
		pre=${s%%$REFCH*}
		[[ $pre == "$s" ]] && return 0
		s=${s:${#pre}}
		case ${s:0:1} in
		'"')
			rest=${s:1}
			[[ $rest == *'"'* ]] || return 0
			q=${rest%%'"'*}
			s=${rest:${#q}+1}
			;;
		';') return 0 ;;
		*)
			if gname "${s:1}"; then want "$R"; s=${s:${#R}}; else s=${s:1}; fi
			;;
		esac
	done
}
mark_ids() {   # the attribute groups / metadata nodes in " id id ..." are needed
	local ix
	for ix in $1; do
		if [[ -n ${META_LINE[ix]} && -z ${NEED[ix]} ]]; then NEED[ix]=1 NSTK[NSP]=$ix; ((NSP++)); fi
	done
}
# script IR file + import index directory -> merged IR file; status 1 when there is nothing to inline
rtl_import() {   # ll index_dir out
	local d=$2 i j l ne st qs qe e nm ix body
	local -a SL OUT BODY XDECL XDREF XBREF XNREF MIX MLINE MREF TYPES BL
	mapfile -t E_NAME < "$d/ix_name" || return 1
	mapfile -t E_KIND < "$d/ix_kind"
	mapfile -t E_NI < "$d/ix_ni"
	mapfile -t E_RD < "$d/ix_rd"
	IDX=() SDECL=() SDEF=() E_ST=() META_LINE=() NEED=() NSTK=() IMPQ=() NSP=0 TOUCHED=()
	ne=${#E_NAME[@]}
	for ((e = 0; e < ne; e++)); do IDX[${E_NAME[e]}]=$e; done
	mapfile -t SL < "$1" || return 1
	for l in "${SL[@]}"; do   # the script: what it defines, what it declares
		case $l in
		"declare "*) gname "${l#*@}" && SDECL[$R]=1 ;;
		"define "*) gname "${l#*@}" && SDEF[$R]=1 ;;
		@*) [[ $l != *' = external '* ]] && gname "${l:1}" && SDEF[$R]=1 ;;
		esac
	done
	DEPTH=0 CLASH=0
	for l in "${SL[@]}"; do   # references from code (noinline calls cannot use an import)
		[[ $l == "declare "* || $l == *' noinline' ]] || each_ref "$l"
	done
	qs=0 DEPTH=1
	if ((${#IMPQ[@]})); then mapfile -t XNREF < "$d/ix_nref"; fi
	while ((qs < ${#IMPQ[@]})); do   # breadth-first over imported bodies, one depth level at a time
		qe=${#IMPQ[@]}
		for ((; qs < qe; qs++)); do
			e=${IMPQ[qs]}
			if [[ ${XNREF[e]} == '!' ]]; then
				body=
				IFS= read -r -d '' body < "$d/b/$e"
				mapfile -t BL <<< "${body%$'\n'}"
				for l in "${BL[@]}"; do each_ref "$l"; done
			else
				for nm in ${XNREF[e]}; do want "$nm"; done
			fi
		done
		((DEPTH++))
	done
	((${#IMPQ[@]} == 0 || CLASH)) && return 1   # nothing to inline
	for l in "${SL[@]}"; do   # script, minus declarations of what is now imported
		if [[ $l == "declare "* ]] && gname "${l#*@}"; then
			e=${IDX[$R]}
			[[ -n $e ]] && ((E_ST[e] == 2)) && continue
		fi
		OUT+=("$l")
	done
	mapfile -t XDECL < "$d/ix_decl"
	mapfile -t XDREF < "$d/ix_dref"
	mapfile -t XBREF < "$d/ix_bref"
	mapfile -t MIX < "$d/meta_ix"
	mapfile -t MLINE < "$d/meta_line"
	mapfile -t MREF < "$d/meta_ref"
	for ((i = 0; i < ${#MIX[@]}; i++)); do META_LINE[MIX[i]]=${MLINE[i]} META_REF[MIX[i]]=${MREF[i]}; done
	for e in "${!TOUCHED[@]}"; do   # imports and declarations, in library order
		if ((E_ST[e] == 1)); then
			[[ -n ${SDECL[${E_NAME[e]}]} ]] && continue   # the script's own declaration stays
			BODY+=("${XDECL[e]}")
			mark_ids "${XDREF[e]}"
		else
			body=
			IFS= read -r -d '' body < "$d/b/$e"
			BODY+=("${body%$'\n'}")
			mark_ids "${XBREF[e]}"
		fi
	done
	mapfile -t TYPES < "$d/types"
	OUT+=("${TYPES[@]}")   # types (all: few, and they nest)
	while ((NSP)); do   # metadata nodes refer to more metadata nodes
		((NSP--))
		ix=${NSTK[NSP]}
		((ix & 1)) && mark_ids "${META_REF[ix]}"
	done
	for ix in "${!NEED[@]}"; do OUT+=("${META_LINE[ix]}"); done
	printf '%s\n' "${OUT[@]}" "${BODY[@]}" > "$3"
}
