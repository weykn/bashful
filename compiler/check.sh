# check.sh - pre-pass: user functions, set -e, literal eval/trap bodies, and detection of anything that cannot
# be compiled exactly (-> whole-script fallback to an embedded bash)
declare -A FBC FBV USERFN FNB_LAST FNB_COUNT
for _x in source . jobs fg bg disown suspend shopt alias unalias type hash compgen complete compopt enable \
	caller ulimit times history fc bind logout help coproc select; do FBC[_$_x]=1; done
# builtins written in bash (shlib/NAME.sh, embedded in the compiler as SHLIB_NAME by the Makefile): a script
# that calls one gets that file's functions, and the call becomes __bashful_b_<builtin>
declare -A SHLIB_OF=([_getopts]=getopts [_pushd]=dirs [_popd]=dirs [_dirs]=dirs) SHLIB_NEED=()
for _x in BASH_LINENO PIPESTATUS BASHPID BASH_VERSINFO BASH_VERSION GROUPS DIRSTACK BASH_ARGV BASH_ARGC \
	BASH_COMMAND BASH_SUBSHELL HISTCMD SHELLOPTS BASHOPTS COPROC BASH_ALIASES BASH_CMDS \
	BASH_EXECUTION_STRING FUNCNEST PROMPT_COMMAND BASH_XTRACEFD SRANDOM BASH_ARGV0 BASH_COMPAT BASH_ENV; do FBV[_$_x]=1; done
NFNB=0 ERREXIT=0

check_var() {
	local b=${1%%'['*}
	((${#b} < 128)) && [[ -n ${FBV[_$b]} ]] && unsupported "\$$b"
}
check_arith_text() {   # identifiers mentioned inside (( )) / $(( ))
	local t=$1 i=0 L=${#1} st nm
	[[ $t == *[A-Z]* ]] || return 0
	while ((i < L)); do
		if [[ ${t:i:1} == [a-zA-Z_] ]]; then
			st=$i
			while [[ ${t:i:1} == [a-zA-Z0-9_] ]]; do ((i++)); done
			nm=${t:st:i-st}
			((i - st < 128)) && [[ -n ${FBV[_$nm]} ]] && unsupported "\$$nm"
		else ((i++)); fi
	done
}
walk_word() {
	local w=$1 p t
	((w)) || return 0
	for p in ${WP[w]}; do
		case ${PK[p]} in
		1) check_var "${PT[p]}" ;;
		2)
			t=${PT[p]}
			check_var "$t"
			walk_word ${PA[p]}
			walk_word ${PC[p]}
			[[ $t == *'['* ]] && check_arith_text "[${t#*'['}"
			;;
		3 | 5) walk ${PB[p]} ;;
		4)
			check_arith_text "${PT[p]}"
			walk_word ${PA[p]}
			;;
		esac
	done
}
sig_ok() {
	local s=${1#SIG}
	[[ $s == 0 ]] && return 0
	if [[ $s == [0-9]* ]]; then
		c_atoi "$s"
		((R > 0 && R < 32 && R != 9 && R != 19))
		return
	fi
	case $s in EXIT | INT | TERM | HUP | QUIT | USR1 | USR2 | PIPE | ALRM | CHLD | WINCH | CONT | TSTP) return 0 ;; esac
	return 1
}
shlib_use() {   # node builtin: call the bash-written version instead
	local n=$1 name=$2 w p
	local -a ws
	ws=(${NWS[n]})
	p=${WL[ws[0]]}
	PT[p]=__bashful_b_$name
	SHLIB_NEED[_${SHLIB_OF[_$name]}]=1
	if [[ $name == getopts ]] && ((${#ws[@]} == 3)); then   # getopts spec name: the caller's "$@"
		mkword
		w=$R
		newpart $PK_PARAM @ 1
		add_part $w $R
		NWS[n]+=" $w"
	fi
}
# the bash-written builtins the script needs, as top-level nodes -> R (" node node ...")
shlib_nodes() {
	local k v nodes= l
	for k in "${!SHLIB_NEED[@]}"; do
		v=SHLIB${k}
		NOLINES=1   # $LINENO and messages inside a builtin show the caller's line
		sub_enter "${!v}"
		parse_list
		l=$R
		sub_leave
		NOLINES=0
		walk $l
		nodes+=${NK[l]}
	done
	R=$nodes
}
walk_simple() {
	local n=$1 name t i nw code body first_sig reset sg ex any asc nm a c ch wants_o opts allowed
	local -a ws
	ws=(${NWS[n]})
	nw=${#ws[@]}
	lit_name ${ws[0]} || return 0
	name=$R
	[[ -n ${USERFN[_$name]} ]] && return 0
	if [[ -n ${SHLIB_OF[_$name]} ]]; then shlib_use $n "$name"; return 0; fi
	if [[ -n ${FBC[_$name]} ]]; then unsupported "'$name' builtin"; return 0; fi
	case $name in
	eval)
		any=0
		for ((i = 1; i < nw; i++)); do word_const ${ws[i]} || any=1; done
		if [[ -n ${NASG[n]} ]] || ((any)); then unsupported "eval of a non-literal string"; return 0; fi
		code=
		for ((i = 1; i < nw; i++)); do
			word_const ${ws[i]}
			((i > 1)) && code+=' '
			code+=$R
		done
		NFLAG[n]=1
		parse_text "$code" "${NLINE[n]}"
		body=$R
		NK[n]=" $body"
		walk $body
		;;
	trap)
		any=0
		for ((i = 1; i < nw; i++)); do word_const ${ws[i]} || any=1; done
		if ((nw < 2 || any)); then unsupported "trap without a literal action"; return 0; fi
		word_const ${ws[1]}
		t=$R
		first_sig=2 body=0 reset=0
		if [[ $t == - ]]; then reset=1
		elif ((nw == 2)) && sig_ok "$t"; then reset=1 first_sig=1
		elif [[ $t == -* ]]; then unsupported "trap $t"; return 0
		elif [[ -n $t ]]; then parse_text "$t" "${NLINE[n]}"; body=$R; fi
		if ((first_sig >= nw)); then unsupported "trap without signals"; return 0; fi
		for ((i = first_sig; i < nw; i++)); do
			word_const ${ws[i]}
			sg=$R
			if ! sig_ok "$sg"; then unsupported "trap on $sg"; return 0; fi
		done
		NFLAG[n]=2
		NK[n]=" $body"
		if ((reset)); then NS1[n]=reset; elif ((body)); then NS1[n]=run; else NS1[n]=ignore; fi
		NS2[n]=$first_sig
		((body)) && walk $body
		;;
	declare | local | typeset | readonly | export)
		ex=0 any=0
		[[ $name == export ]] && ex=1
		for ((i = 1; i < nw; i++)); do
			if ! word_const ${ws[i]}; then any=1; continue; fi
			t=$R
			if [[ $t == [-+]?* ]]; then
				[[ $t == -- ]] && continue
				opts=${t:1}
				if ((ex)); then [[ $opts == *[!n]* ]] && { unsupported "$name $t"; return 0; }
				else [[ $opts == *[!agxrA]* ]] && { unsupported "$name $t"; return 0; }; fi
			else any=1; fi
		done
		asc=0   # declare -A names (must be literal)
		for ((i = 1; i < nw; i++)); do
			if ! word_const ${ws[i]}; then
				if ((asc)); then unsupported "$name -A with a computed name"; return 0; fi
				continue
			fi
			t=$R
			if [[ $t == -?* ]]; then [[ $t == *A* ]] && asc=1; continue; fi
			if [[ $t == +* && $t == *A* ]]; then unsupported "$name +A"; return 0; fi
			if ((asc)); then add_assoc "${t%%[=+[]*}"; fi
		done
		if ((asc)); then for a in ${NARR[n]}; do add_assoc "${AN[a]}"; done; fi
		[[ $any == 0 && -z ${NARR[n]} ]] && unsupported "$name without names (prints variables)"
		;;
	set)
		for ((i = 1; i < nw; i++)); do
			word_const ${ws[i]} || break
			t=$R
			[[ $t == -- || $t == - ]] && break
			[[ $t == [-+]?* ]] || break
			wants_o=0
			opts=${t:1}
			for ((c = 0; c < ${#opts}; c++)); do
				ch=${opts:c:1}
				if [[ $ch == o ]]; then wants_o=1; continue; fi
				if [[ $ch != [eu] ]]; then unsupported "set ${t:0:1}$ch"; return 0; fi
			done
			if ((wants_o)); then
				if ((i + 1 < nw)) && word_const ${ws[i + 1]} && [[ $R == pipefail || $R == errexit || $R == nounset ]]; then
					((i++))
					continue
				fi
				unsupported "set -o option"
				return 0
			fi
		done
		((nw == 1)) && unsupported "set without arguments"
		;;
	read | mapfile | readarray)
		if [[ $name == read ]]; then allowed=rsadpnNtu opts=adpnNtu; else allowed=tdns opts=dns; fi
		for ((i = 1; i < nw; i++)); do
			word_const ${ws[i]} || break
			t=$R
			[[ $t == -?* ]] || break
			[[ $name == read && $t == -- ]] && break
			for ((c = 1; c < ${#t}; c++)); do
				ch=${t:c:1}
				if [[ $allowed != *"$ch"* ]]; then unsupported "$name -$ch"; return 0; fi
				if [[ $opts == *"$ch"* ]]; then
					((c + 1 >= ${#t})) && ((i++))
					break
				fi
			done
		done
		;;
	unset)
		for ((i = 1; i < nw; i++)); do
			if word_const ${ws[i]} && [[ $R == -* && $R != -v ]]; then unsupported "unset $R"; return 0; fi
		done
		;;
	wait)
		for ((i = 1; i < nw; i++)); do
			if word_const ${ws[i]} && [[ $R == -* ]]; then unsupported "wait $R"; return 0; fi
		done
		;;
	esac
	return 0
}
walk() {
	local n=$1 ty i a w r ci k t nm
	local -a ws
	((n)) || return 0
	ty=${NT[n]}
	if ((ty == N_FUNC)); then
		nm=${NNAME[n]}
		USERFN[_$nm]=1
		k=${NK[n]# }
		FNB_BODY[NFNB]=${k%% *}
		FNB_NAME[NFNB]=$nm
		FNB_LAST[_$nm]=$NFNB
		((FNB_COUNT[_$nm]++))
		((NFNB++))
	fi
	if ((ty == N_SIMPLE)) && [[ -n ${NWS[n]} ]]; then
		ws=(${NWS[n]})
		if lit_name ${ws[0]} && [[ $R == set ]]; then
			for ((i = 1; i < ${#ws[@]}; i++)); do
				lit_name ${ws[i]} || continue
				t=$R
				if [[ $t == -* && ${t:1:1} != - && $t == *e* ]] || [[ $t == errexit ]]; then ERREXIT=1; fi
			done
		fi
	fi
	for w in ${NWS[n]}; do walk_word $w; done
	for a in ${NASG[n]}; do
		walk_word ${AVAL[a]}
		for w in ${AEL[a]}; do walk_word $w; done
	done
	for a in ${NARR[n]}; do
		walk_word ${AVAL[a]}
		for w in ${AEL[a]}; do walk_word $w; done
	done
	for r in ${NRS[n]}; do walk_word ${RT[r]}; done
	for ci in ${NCIS[n]}; do for w in ${CIP[ci]}; do walk_word $w; done; done
	walk_word ${NAW0[n]}
	walk_word ${NAW1[n]}
	walk_word ${NAW2[n]}
	if ((ty == N_ARITH || ty == N_CFOR)); then
		check_arith_text "${NS1[n]}"
		check_arith_text "${NS2[n]}"
		check_arith_text "${NS3[n]}"
	fi
	if ((ty == N_SIMPLE)) && [[ -n ${NWS[n]} ]] && ((NFLAG[n] == 0)); then walk_simple $n; fi
	((ty == N_SIMPLE && NFLAG[n])) && return 0   # eval/trap bodies were walked when parsed
	for k in ${NK[n]}; do walk $k; done
	for ci in ${NCIS[n]}; do walk ${CIB[ci]}; done
	return 0
}
