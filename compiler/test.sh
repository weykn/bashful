# test.sh - native [ ] / [[ ]]
# Common tests become direct comparisons: integers are read from the variable's cached integer, strings are
# compared via borrowed pointers.  Anything exotic fails and the generic path is used.
#
# State (saved/restored by native_test, which can nest through $(...) operands): TCW words, TCN count, TCI
# position, TCD [[ ]], TCF failed, TCU uses integer comparisons of [ ].  tc_str leaves OWN (operand must be freed).
TCN=0 TCI=0 TCD=0 TCF=0 TCU=0

tc_lit() { (($1 < TCN)) && lit_name ${TCW[$1]}; }
is_cmp_int() { case $1 in -eq | -ne | -lt | -le | -gt | -ge) return 0 ;; esac; return 1; }
is_cmp_str() { [[ $1 == = || $1 == == || $1 == != ]]; }
is_bin_any() {
	case $1 in = | == | != | -eq | -ne | -lt | -le | -gt | -ge | '<' | '>' | -nt | -ot | -ef) return 0 ;; esac
	((TCD)) && [[ $1 == =~ ]]
}
is_file_un() { [[ $1 == -[efdrwxsLhpSbcgukOGNtv] ]]; }
has_glob() { [[ $1 == *'*'* || $1 == *'?'* || $1 == *'['* || $1 == *']'* || $1 == *'\'* ]]; }
tc_str() {   # word pat -> R = string operand, OWN
	local w=$1 pat=$2 p allq=1 t
	OWN=0
	p=${WL[w]}
	# a lone variable is borrowed as is: as a string when quoted (or inside [[ ]]), as a pattern only when unquoted
	if ((WN[w] == 1 && PK[p] == 1 && (pat ? !PQ[p] : (PQ[p] || TCD)))) && is_ident "${PT[p]}" && ! special_kind "${PT[p]}"; then
		t=%t$((TMP++))
		vid "${PT[p]}"
		emit "$t = call ptr @bash_rt_gets_e(i32 $R)"
		R=$t
		return
	fi
	for p in ${WP[w]}; do ((PK[p])) && allq=0; done
	if ((allq && WN[w] && !pat)) && word_const $w; then S "$R"; return; fi
	if ((allq && WN[w] && pat)) && opd_const $w 1; then S "$R"; return; fi
	if ((!TCD)); then
		for p in ${WP[w]}; do
			if ((!PQ[p] && PK[p] != 0)); then TCF=1 R=null; return; fi
		done
	fi
	if ((pat)); then word_to_str $w $WB_PATTERN; else word_to_str $w $WB_STR; fi
	OWN=1 R=%t$R
}
tc_int() {   # word -> R = i64 operand
	local w=$1 p nm t sfx=_t
	((TCD)) && sfx=
	if word_const $w; then
		if strtol_full "$R"; then return; fi
		TCF=1 R=0
		return
	fi
	p=${WL[w]}
	if ((WN[w] == 1 && PK[p] == 1)); then
		nm=${PT[p]}
		t=%t$((TMP++))
		if [[ $nm == [0-9]* ]] && c_atoi "$nm" && ((R > 0)); then
			emit "$t = call i64 @bash_rt_pos_i$sfx(i32 $R)"
			((TCD)) && TCF=1
			R=$t
			return
		fi
		if is_ident "$nm" && ! special_kind "$nm"; then
			vid "$nm"
			emit "$t = call i64 @bash_rt_geti$sfx(i32 $R)"
			R=$t
			return
		fi
		if [[ $nm == '#' ]]; then emit "$t = call i64 @bash_rt_special_i(i32 2)"; R=$t; return; fi
		if [[ $nm == '?' ]]; then emit "$t = call i64 @bash_rt_special_i(i32 1)"; R=$t; return; fi
	fi
	if ((WN[w] == 1 && PK[p] == 4)) && arith_native "${PT[p]}"; then return; fi
	TCF=1 R=0
}
tc_atom() {
	local rem=$((TCN - TCI)) a0= ha0=0 a1= ha1=0 lw rw r x y cc neg own1 own2 glob m p q pb i ch w op b v
	if ((rem <= 0)); then TCF=1 R=0; return; fi
	tc_lit $TCI && a0=$R ha0=1
	if ((ha0 && rem >= 2 && TCD)) && [[ $a0 == '(' ]]; then
		((TCI++))
		tc_or
		v=$R
		if tc_lit $TCI && [[ $R == ')' ]]; then ((TCI++)); else TCF=1; fi
		R=$v
		return
	fi
	((rem >= 2)) && tc_lit $((TCI + 1)) && a1=$R ha1=1
	if ((rem >= 3 && ha1)) && is_bin_any "$a1"; then
		lw=${TCW[TCI]} rw=${TCW[TCI + 2]}
		((TCI += 3))
		r=%t$((TMP++))
		if is_cmp_int "$a1"; then
			((TCD)) || TCU=1
			tc_int $lw
			x=$R
			tc_int $rw
			y=$R
			case $a1 in -eq) cc=eq ;; -ne) cc=ne ;; -lt) cc=slt ;; -le) cc=sle ;; -gt) cc=sgt ;; *) cc=sge ;; esac
			emit "$r = icmp $cc i64 $x, $y"
			R=$r
			return
		fi
		if is_cmp_str "$a1"; then
			neg=0
			[[ $a1 == !* ]] && neg=1
			tc_str $lw 0
			x=$R own1=$OWN
			glob=0   # quoted parts match literally: only unquoted expansions and glob characters make a pattern
			if ((TCD)); then
				for p in ${WP[rw]}; do
					((PQ[p])) && continue
					if ((PK[p] != 0)); then glob=1
					elif has_glob "${PT[p]}"; then glob=1; fi
				done
			fi
			tc_str $rw $((TCD && glob))
			y=$R own2=$OWN
			m=%t$((TMP++))
			if ((TCD && glob)); then emit "$m = call i32 @bash_rt_fnm(ptr $y, ptr $x)"; else emit "$m = call i32 @bash_rt_streq(ptr $x, ptr $y)"; fi
			((own1)) && emit "call void @free(ptr $x)"
			((own2)) && emit "call void @free(ptr $y)"
			if ((neg)); then emit "$r = icmp eq i32 $m, 0"; else emit "$r = icmp ne i32 $m, 0"; fi
			R=$r
			return
		fi
		if ((TCD)) && [[ $a1 == =~ ]]; then
			# constant pattern: unquoted text is regex syntax, quoted text matches literally
			pb=
			((WN[rw])) || TCF=1
			for p in ${WP[rw]}; do
				((TCF)) && break
				if ((PK[p] != 0)); then TCF=1; break; fi
				if ((!PQ[p])); then pb+=${PT[p]}; continue; fi
				q=${PT[p]}
				for ((i = 0; i < ${#q}; i++)); do
					ch=${q:i:1}
					case $ch in '\' | . | '[' | ']' | '(' | ')' | '*' | + | '?' | '{' | '}' | '|' | ^ | '$') pb+='\' ;; esac
					pb+=$ch
				done
			done
			if ((TCF)); then R=0; return; fi
			tc_str $lw 0
			x=$R own1=$OWN
			m=%t$((TMP++))
			S "$pb"
			emit "$m = call i32 @bash_rt_regex(ptr $x, ptr $R)"
			((own1)) && emit "call void @free(ptr $x)"
			emit "$r = icmp ne i32 $m, 0"
			R=$r
			return
		fi
		TCF=1 R=0
		return
	fi
	if ((rem >= 2 && ha0)) && is_file_un "$a0"; then
		w=${TCW[TCI + 1]}
		op=${a0:1:1}
		((TCI += 2))
		tc_str $w 0
		x=$R own1=$OWN
		r=%t$((TMP++))
		if [[ $op == v ]]; then TCF=1 R=$r; return; fi
		m=%t$((TMP++))
		ord "$op"
		emit "$m = call i32 @bash_rt_ftest(i32 $R, ptr $x)"
		((own1)) && emit "call void @free(ptr $x)"
		emit "$r = icmp ne i32 $m, 0"
		R=$r
		return
	fi
	if ((rem >= 2 && ha0)) && [[ $a0 == -z || $a0 == -n ]]; then
		w=${TCW[TCI + 1]}
		((TCI += 2))
		tc_str $w 0
		x=$R own1=$OWN
		b=%t$((TMP++)) r=%t$((TMP++))
		emit "$b = load i8, ptr $x"
		if [[ $a0 == -z ]]; then emit "$r = icmp eq i8 $b, 0"; else emit "$r = icmp ne i8 $b, 0"; fi
		((own1)) && emit "call void @free(ptr $x)"
		R=$r
		return
	fi
	if ((rem == 1 || TCD)); then   # lone string: true when non-empty
		if ((ha0)) && { is_bin_any "$a0" || is_file_un "$a0" || [[ $a0 == '!' || $a0 == '(' || $a0 == -z || $a0 == -n ]]; }; then TCF=1 R=0; return; fi
		w=${TCW[TCI]}
		((TCI++))
		tc_str $w 0
		x=$R own1=$OWN
		b=%t$((TMP++)) r=%t$((TMP++))
		emit "$b = load i8, ptr $x"
		emit "$r = icmp ne i8 $b, 0"
		((own1)) && emit "call void @free(ptr $x)"
		R=$r
		return
	fi
	TCF=1 R=0
}
tc_not() {
	local v r
	if tc_lit $TCI && [[ $R == '!' ]] && ((TCN - TCI >= 2)); then
		((TCI++))
		tc_not
		v=$R
		r=%t$((TMP++))
		emit "$r = xor i1 $v, true"
		R=$r
		return
	fi
	tc_atom
}
tc_andor() {   # is_and: && / -a over tc_not, else || / -o over the and level
	local is_and=$1 l o id res rhs en lz r rz ld b want
	if ((is_and)); then
		tc_not
		if ((TCD)); then want='&&'; else want=-a; fi
	else
		tc_andor 1
		if ((TCD)); then want='||'; else want=-o; fi
	fi
	l=$R
	while :; do
		tc_lit $TCI && [[ $R == "$want" ]] || { R=$l; return; }
		((TCI++))
		id=$((LBL++))
		alloca_i32
		res=$R
		if ((is_and)); then rhs=tc.rhs.$id en=tc.end.$id; else rhs=tc.orhs.$id en=tc.oend.$id; fi
		lz=%t$((TMP++))
		emit "$lz = zext i1 $l to i32"
		emit "store i32 $lz, ptr %t$res"
		if ((is_and)); then cbr ${l#%t} $rhs $en; else cbr ${l#%t} $en $rhs; fi
		label $rhs
		if ((is_and)); then tc_not; else tc_andor 1; fi
		r=$R
		rz=%t$((TMP++))
		emit "$rz = zext i1 $r to i32"
		emit "store i32 $rz, ptr %t$res"
		br $en
		label $en
		ld=%t$((TMP++)) b=%t$((TMP++))
		emit "$ld = load i32, ptr %t$res"
		emit "$b = icmp ne i32 $ld, 0"
		l=$b
	done
}
tc_or() { tc_andor 0; }
# emits the test, sets $?; R = i1 temp number (true == success)
native_test() {
	local n=$1 ty=${NT[$1]} dbl=0 nw i v st cd mark term sN sI sD sF sU
	local -a ws tw sW
	ws=(${NWS[n]})
	nw=${#ws[@]}
	if ((ty == N_DBL)); then
		tw=("${ws[@]}")
		dbl=1
	elif ((ty == N_SIMPLE && nw >= 1)) && [[ -z ${NRS[n]} && -z ${NASG[n]} && -z ${NARR[n]} ]] && lit_name ${ws[0]} && ! is_user_fn "$R" && [[ $R == '[' || $R == test ]]; then
		if [[ $R == '[' ]]; then
			((nw >= 2)) && lit_name ${ws[nw - 1]} && [[ $R == ']' ]] || return 1
			for ((i = 1; i < nw - 1; i++)); do tw+=(${ws[i]}); done
		else
			for ((i = 1; i < nw; i++)); do tw+=(${ws[i]}); done
		fi
	else return 1; fi
	((${#tw[@]})) || return 1
	[[ -z ${NRS[n]} ]] || return 1
	sW=("${TCW[@]}")
	sN=$TCN sI=$TCI sD=$TCD sF=$TCF sU=$TCU
	TCW=("${tw[@]}")
	TCN=${#tw[@]} TCI=0 TCD=$dbl TCF=0 TCU=0
	mark=${#FB} term=$FT
	if ((!dbl)); then
		for ((i = 0; i < TCN; i++)); do lit_name ${TCW[i]} && is_cmp_int "$R" && TCU=1; done
	fi
	((TCU)) && emit "call void @bash_rt_test_err_clear()"
	tc_or
	v=$R
	if ((TCF || TCI != TCN)); then
		FB=${FB:0:mark} FT=$term
		TCW=("${sW[@]}")
		TCN=$sN TCI=$sI TCD=$sD TCF=$sF TCU=$sU
		return 1
	fi
	st=$((TMP++)) cd=$((TMP++))
	if ((TCU)); then
		emit "%t$st = call i32 @bash_rt_test_status(i1 $v)"
		emit "call void @bash_rt_set_status(i32 %t$st)"
		emit "%t$cd = icmp eq i32 %t$st, 0"
	else
		emit "%t$st = select i1 $v, i32 0, i32 1"
		emit "call void @bash_rt_set_status(i32 %t$st)"
		cd=${v#%t}
	fi
	TCW=("${sW[@]}")
	TCN=$sN TCI=$sI TCD=$sD TCF=$sF TCU=$sU
	R=$cd
	return 0
}
