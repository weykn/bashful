# gen.sh - code generation for commands and control flow
declare -A STDIN_R
for _x in cat grep sort uniq tr cut head tail wc tee read mapfile readarray; do STDIN_R[_$_x]=1; done

errexit_check() { ((ERREXIT && FCOND == 0)) && emit "call void @bash_rt_errexit_check()"; return 0; }
first_child() { R=${NK[$1]# }; R=${R%% *}; }

gen_trap() {
	local n=$1 first=${NS2[$1]} mode f i
	local -a ws
	case ${NS1[n]} in reset) mode=0 ;; ignore) mode=1 ;; *) mode=2 ;; esac
	if ((mode == 2)); then first_child $n; gen_block_fn $R; f=$R; else f=null; fi
	ws=(${NWS[n]})
	for ((i = first; i < ${#ws[@]}; i++)); do
		word_const ${ws[i]}
		S "$R"
		emit "call void @bash_rt_trap(ptr $R, i32 $mode, ptr $f)"
	done
	set_status_const 0
}
# local/declare/typeset/export/readonly argument that is a plain name or name=value: DN, DV (word or 0), DAPP
decl_assign_word() {
	local w=$1 p0 t j=0 ap=0 v rest k c
	local -a ps
	((WN[w])) || return 1
	ps=(${WP[w]})
	p0=${ps[0]}
	((PK[p0] == 0 && !PQ[p0])) || return 1
	t=${PT[p0]}
	[[ ${t:0:1} == [a-zA-Z_] ]] || return 1
	while [[ ${t:j:1} == [a-zA-Z0-9_] ]]; do ((j++)); done
	if [[ -z ${t:j:1} ]] && ((WN[w] == 1)); then DN=${t:0:j} DV=0 DAPP=0; return 0; fi
	if [[ ${t:j:2} == += ]]; then ap=1; ((j++)); fi
	[[ ${t:j:1} == = ]] || return 1
	DN=${t:0:j-ap} DAPP=$ap
	mkword
	v=$R
	rest=${t:j+1}
	if [[ ${rest:0:1} == '~' ]] && [[ -z ${rest:1:1} || ${rest:1:1} == / || ${rest:1:1} == : ]]; then
		newpart $PK_VAR HOME 1
		add_part $v $R
		rest=${rest:1}
	fi
	[[ -n $rest ]] && add_lit $v "$rest" 1 0
	for ((k = 1; k < ${#ps[@]}; k++)); do
		c=${ps[k]}
		if ((PK[c] == 0)); then part_copy $c; c=$R; PQ[c]=1; fi
		add_part $v $c
	done
	((WN[v])) || add_lit $v "" 1 1
	DV=$v
	return 0
}
gen_native_decl() {   # node name -> status 0 when handled
	local n=$1 name=$2 loc=0 ex=0 ro=0 asc=0 first=1 i t c ch id sv vw nm sk tt borrow dolocal a p nw
	local -a ws narr dn dv dap
	case $name in local | declare | typeset) loc=1 ;; export) ex=1 ;; readonly) ro=1 ;; *) return 1 ;; esac
	[[ $name == local ]] && ((!IN_FUNC)) && return 1
	[[ -n ${NRS[n]} || -n ${NASG[n]} ]] && return 1
	ws=(${NWS[n]})
	narr=(${NARR[n]})
	nw=${#ws[@]}
	((nw + ${#narr[@]} < 2)) && return 1
	for ((; first < nw && loc; first++)); do   # leading literal options: -a -A -g -x -r (declare/local/typeset only)
		lit_name ${ws[first]} || break
		t=$R
		[[ $t == -?* ]] || break
		((PQ[WL[ws[first]]])) && break
		if [[ $t == -- ]]; then ((first++)); break; fi
		for ((c = 1; c < ${#t}; c++)); do
			ch=${t:c:1}
			case $ch in
			A) asc=1 ;;
			g) if [[ $name != local ]]; then loc=0; else return 1; fi ;;
			x) ex=1 ;;
			r) ro=1 ;;
			a) ;;
			*) return 1 ;;
			esac
		done
	done
	((first >= nw && ${#narr[@]} == 0)) && return 1
	for ((i = first; i < nw; i++)); do
		decl_assign_word ${ws[i]} || return 1
		special_kind "$DN" && return 1
		dn[i]=$DN dv[i]=$DV dap[i]=$DAPP
	done
	for a in "${narr[@]}"; do is_ident "${AN[a]}" || return 1; done
	dolocal=$((loc && IN_FUNC))   # declare at top level is just a global
	for ((i = first; i < nw; i++)); do
		vid "${dn[i]}"
		id=$R
		if ((asc)); then
			((dolocal)) && emit "call void @bash_rt_local_id(i32 $id)"
			emit "call void @bash_rt_assoc_id(i32 $id)"
		fi
		if ((dv[i])); then
			vw=${dv[i]}
			p=${WL[vw]}
			borrow=0
			((WN[vw] == 1 && PK[p] == 1 && !dap[i])) && borrow=1
			if ((borrow)); then   # local x=$1 / local x=$y: the value is read before the local shadows anything
				nm=${PT[p]}
				if special_kind "$nm"; then borrow=0
				elif [[ $nm == [0-9]* ]]; then
					tt=$((TMP++))
					c_atoi "$nm"
					emit "%t$tt = call ptr @bash_rt_pos_u(i32 $R)"
				else
					vid "$nm"
					if ((R == id)); then borrow=0   # local x=$x
					else
						tt=$((TMP++))
						emit "%t$tt = call ptr @bash_rt_gets_e(i32 $R)"
					fi
				fi
				if ((borrow)); then
					((dolocal && !asc)) && emit "call void @bash_rt_local_id(i32 $id)"
					if [[ $nm == [0-9]* ]]; then emit "call void @bash_rt_set_id(i32 $id, ptr %t$tt, i32 0)"
					else vid "$nm"; emit "call void @bash_rt_copy_id(i32 $id, i32 $R)"; fi
				fi
			fi
			if ((!borrow)); then
				word_to_str ${dv[i]} $WB_STR
				sv=$R
				((dolocal && !asc)) && emit "call void @bash_rt_local_id(i32 $id)"
				emit "call void @bash_rt_set_id(i32 $id, ptr %t$sv, i32 ${dap[i]})"
				emit "call void @free(ptr %t$sv)"
			fi
		elif ((dolocal && !asc)); then emit "call void @bash_rt_local_id(i32 $id)"
		fi
		((ex)) && emit "call void @bash_rt_export_id(i32 $id)"
		((ro)) && emit "call void @bash_rt_readonly_id(i32 $id)"
	done
	for a in "${narr[@]}"; do
		if ((dolocal)); then vid "${AN[a]}"; emit "call void @bash_rt_local_id(i32 $R)"; fi
		if ((asc)); then vid "${AN[a]}"; emit "call void @bash_rt_assoc_id(i32 $R)"; fi
		gen_assign $a 0
		if ((ex)); then vid "${AN[a]}"; emit "call void @bash_rt_export_id(i32 $R)"; fi
		if ((ro)); then vid "${AN[a]}"; emit "call void @bash_rt_readonly_id(i32 $R)"; fi
	done
	set_status_const 0
	return 0
}
gen_simple() {
	local n=$1 name= hasname=0 ra rf rd mark=-1 b argc argv a r ti hs=0 s loc lvl di d a2 b2 id ld ls le w nw t nl
	local -a ws rs
	if ((NFLAG[n] == 1)); then   # eval 'literal'
		redir_begin "${NRS[n]}"
		ra=$RC_A rf=$RC_F rd=$RC_D
		first_child $n
		gen_node $R
		redir_end $ra $rf $rd
		return
	fi
	if ((NFLAG[n] == 2)); then gen_trap $n; return; fi
	((NLINE[n])) && emit "call void @bash_rt_lineno(i32 ${NLINE[n]})"   # $LINENO, "script: line N:" in messages
	ws=(${NWS[n]})
	nw=${#ws[@]}
	if ((nw)) && lit_name ${ws[0]}; then name=$R hasname=1; fi
	if ((hasname)) && ! is_user_fn "$name" && gen_native_decl $n "$name"; then return; fi
	if ((nw == 0)); then
		if [[ -z ${NASG[n]} && -z ${NARR[n]} ]]; then
			redir_begin "${NRS[n]}"
			redir_end $RC_A $RC_F $RC_D
			set_status_const 0
			return
		fi
		emit "call void @bash_rt_cmdsub_reset()"
		for a in ${NASG[n]}; do gen_assign $a 0; done
		t=$((TMP++))
		emit "%t$t = call i32 @bash_rt_cmdsub_status()"
		emit "call void @bash_rt_set_status(i32 %t$t)"
		return
	fi
	if ((hasname && nw == 1)) && [[ $name == exec ]]; then   # exec >file : permanent redirections
		if [[ -n ${NRS[n]} ]]; then
			emit "call void @bash_rt_redir_push()"
			for r in ${NRS[n]}; do
				word_to_str ${RT[r]} $WB_STR
				s=$R
				emit "call void @bash_rt_redir(i32 ${RFD[r]}, i32 ${ROP[r]}, ptr %t$s)"
				emit "call void @free(ptr %t$s)"
			done
			emit "call void @bash_rt_redir_commit()"
		fi
		set_status_const 0
		return
	fi
	nl=$((NLOOPS - LOOPBASE))
	if ((hasname && nl > 0)) && [[ $name == break || $name == continue ]]; then
		lvl=1
		((nw > 1)) && lit_name ${ws[1]} && c_atoi "$R" && lvl=$R
		((lvl < 1)) && lvl=1
		((lvl > nl)) && lvl=$nl
		if [[ $name == break ]]; then br ${LBRK[NLOOPS - lvl]}; else br ${LCONT[NLOOPS - lvl]}; fi
		return
	fi
	if [[ -n ${NASG[n]} ]]; then
		mark=$((TMP++))
		emit "%t$mark = call i32 @bash_rt_tmp_mark()"
		for a in ${NASG[n]}; do gen_assign $a 1; done
	fi
	new_wb $WB_ARGV
	b=$R
	for w in "${ws[@]}"; do emit_word $b $w $WB_ARGV; done
	argc=$((TMP++)) argv=$((TMP++))
	emit "%t$argc = call i32 @bash_rt_wb_argc(ptr %t$b)"
	emit "%t$argv = call ptr @bash_rt_wb_argv(ptr %t$b)"
	if [[ -n ${NARR[n]} ]]; then
		loc=0
		((hasname)) && [[ $name == local || $name == declare || $name == typeset ]] && loc=1
		for a in ${NARR[n]}; do
			if ((loc)); then S "${AN[a]}"; emit "call void @bash_rt_declare_local(ptr $R)"; fi
			gen_assign $a 0
		done
	fi
	if ((hasname)) && [[ $name == return ]]; then
		r=$((TMP++))
		emit "%t$r = call i32 @bash_rt_return_status(i32 %t$argc, ptr %t$argv)"
		emit "call void @bash_rt_wb_free(ptr %t$b)"
		br $FRET
		return
	fi
	# read ... <<< word: hand the string to read directly (no memfd / dup2 / seek)
	if ((hasname)) && [[ $name == read ]] && ! is_user_fn read; then
		rs=(${NRS[n]})
		((${#rs[@]} == 1 && ROP[rs[0]] == R_HERESTR && RFD[rs[0]] == 0)) && hs=1
	fi
	if ((hs)); then
		word_to_str ${RT[rs[0]]} $WB_STR
		emit "call void @bash_rt_read_src(ptr %t$R)"
		redir_begin ""
	else redir_begin "${NRS[n]}"; fi
	ra=$RC_A rf=$RC_F rd=$RC_D
	r=$((TMP++))
	ti=-1
	if ((NFLAG[n] == 3)) || { ((hasname)) && ! is_user_fn "$name"; }; then tool_index "$name"; ti=$R; fi
	if ((ti >= 0)) && [[ ${TOOL_PFX[ti]} == bash_external_ ]]; then   # embedded tool; unsupported options re-run the real program
		USED[ti]=1
		emit "%t$r = call i32 @bash_rt_tool_call(i32 $ti, ptr @${TOOL_PFX[ti]}${TOOL_ID[ti]}, i32 %t$argc, ptr %t$argv)"
	elif ((ti >= 0)); then
		USED[ti]=1
		emit "%t$r = call i32 @${TOOL_PFX[ti]}${TOOL_ID[ti]}(i32 %t$argc, ptr %t$argv)"
	elif ((NFLAG[n] == 3)); then   # $(<file) with --no-embed: the real cat, never a function
		emit "%t$r = call i32 @bash_rt_run_nofunc(i32 %t$argc, ptr %t$argv)"
	elif ((hasname)) && is_user_fn "$name" && [[ -n ${DIRECTIX[_$name]} ]]; then
		di=${DIRECTIX[_$name]}
		d=$((TMP++)) a2=$((TMP++)) b2=$((TMP++)) id=$((LBL++))
		ld=uf.d.$id ls=uf.s.$id le=uf.e.$id
		emit "%t$d = load i1, ptr @bash_ufd_$di"
		cbr $d $ld $ls
		label $ld
		emit "%t$a2 = call i32 @bash_ufn_$di(i32 %t$argc, ptr %t$argv)"
		br $le
		label $ls
		emit "%t$b2 = call i32 @bash_rt_run(i32 %t$argc, ptr %t$argv)"
		br $le
		label $le
		emit "%t$r = phi i32 [ %t$a2, %$ld ], [ %t$b2, %$ls ]"
	else
		emit "%t$r = call i32 @bash_rt_run(i32 %t$argc, ptr %t$argv)"
	fi
	emit "call void @bash_rt_set_status(i32 %t$r)"
	((hs)) && emit "call void @bash_rt_read_src(ptr null)"
	redir_end $ra $rf $rd
	emit "call void @bash_rt_wb_free(ptr %t$b)"
	((mark >= 0)) && emit "call void @bash_rt_tmp_restore(i32 %t$mark)"
	return 0
}

# evaluates n as a condition: R = i1 temp (true == status 0); $? is set as a side effect
gen_cond() {
	local n=$1 c x r st s
	((FCOND++))
	if native_test $n || arith_cond $n; then
		c=$R
		((FCOND--))
		R=$c
		return
	fi
	if ((NT[n] == N_NOT)); then
		first_child $n
		gen_cond $R
		x=$R
		r=$((TMP++)) st=$((TMP++))
		((FCOND--))
		emit "%t$r = xor i1 %t$x, true"
		emit "%t$st = select i1 %t$r, i32 0, i32 1"
		emit "call void @bash_rt_set_status(i32 %t$st)"
		R=$r
		return
	fi
	gen_node $n
	((FCOND--))
	get_status
	s=$R
	c=$((TMP++))
	emit "%t$c = icmp eq i32 %t$s, 0"
	R=$c
}
arith_cond() {
	local n=$1 nv z st
	((NT[n] == N_ARITH)) && [[ -z ${NRS[n]} ]] || return 1
	arith_native "${NS1[n]}" || return 1
	nv=$R
	z=$((TMP++)) st=$((TMP++))
	emit "%t$z = icmp ne i64 $nv, 0"
	emit "%t$st = select i1 %t$z, i32 0, i32 1"
	emit "call void @bash_rt_set_status(i32 %t$st)"
	R=$z
}
# Can this pipeline stage run to completion on its own (in-process, output buffered for the next stage)?
# Requires: every command known at compile time (builtin, embedded tool, or such a function), no loops that
# could be infinite, and -- for the first stage -- nothing that reads the script's own stdin.
node_safe() {   # node first depth
	local n=$1 first=$2 depth=$3 i w a nm bi files has_in r k ci
	local -a ws
	((n)) || return 0
	((depth > 8)) && return 1
	case ${NT[n]} in
	7 | 8 | 10 | 17 | 18) return 1 ;;   # while until for(( )) & time
	0)
		for w in ${NWS[n]}; do word_safe $w $depth || return 1; done
		for a in ${NASG[n]}; do word_safe ${AVAL[a]} $depth || return 1; done
		if ((NFLAG[n] == 1)); then first_child $n; node_safe $R $first $((depth + 1)); return; fi
		[[ -n ${NWS[n]} ]] || return 0
		ws=(${NWS[n]})
		lit_name ${ws[0]} || return 1
		nm=$R
		if is_user_fn "$nm"; then
			bi=${FNB_LAST[_$nm]}
			[[ -n $bi ]] || return 1
			node_safe ${FNB_BODY[bi]} $first $((depth + 1))
			return
		fi
		tool_index "$nm"
		((R < 0)) && return 1
		case $nm in exec | command | builtin | env | sleep | eval | wait) return 1 ;; esac
		if ((first)) && [[ -n ${STDIN_R[_$nm]} ]]; then
			files=0
			for ((i = 1; i < ${#ws[@]}; i++)); do
				if ! lit_name ${ws[i]} || [[ $R != -* ]]; then ((files++)); fi
			done
			has_in=0
			for r in ${NRS[n]}; do ((RFD[r] == 0)) && has_in=1; done
			if ((!has_in)); then
				if ((files == 0)) || [[ $nm == tr || $nm == read || $nm == mapfile || $nm == readarray ]]; then return 1; fi
				[[ $nm == grep ]] && ((files < 2)) && return 1
			fi
		fi
		return 0
		;;
	*)
		for k in ${NK[n]}; do node_safe $k $first $depth || return 1; done
		for ci in ${NCIS[n]}; do node_safe ${CIB[ci]} $first $depth || return 1; done
		for w in ${NWS[n]}; do word_safe $w $depth || return 1; done
		return 0
		;;
	esac
}
word_safe() {   # word depth
	local w=$1 depth=$2 p
	((w)) || return 0
	for p in ${WP[w]}; do
		if ((PK[p] == 3 || PK[p] == 5)); then node_safe ${PB[p]} 0 $((depth + 1)) || return 1; fi
		word_safe ${PA[p]} $depth || return 1
		word_safe ${PC[p]} $depth || return 1
	done
	return 0
}
gen_pipe() {
	local n=$1 k i inproc=1 arr p r
	local -a ks names
	ks=(${NK[n]})
	k=${#ks[@]}
	for ((i = 0; i < k - 1; i++)); do node_safe ${ks[i]} $((i == 0)) 0 || inproc=0; done
	for ((i = 0; i < k; i++)); do gen_block_fn ${ks[i]}; names[i]=$R; done
	arr=$((TMP++))
	FA+="  %t$arr = alloca [$k x ptr]"$'\n'
	for ((i = 0; i < k; i++)); do
		p=$((TMP++))
		emit "%t$p = getelementptr inbounds [$k x ptr], ptr %t$arr, i64 0, i64 $i"
		emit "store ptr ${names[i]}, ptr %t$p"
	done
	r=$((TMP++))
	if ((inproc)); then emit "%t$r = call i32 @bash_rt_pipeline(i32 $k, ptr %t$arr)"
	else emit "%t$r = call i32 @bash_rt_pipeline_fork(i32 $k, ptr %t$arr)"; fi
	emit "call void @bash_rt_set_status(i32 %t$r)"
}
gen_if() {
	local n=$1 id=$((LBL++)) c th el en
	local -a ks
	ks=(${NK[n]})
	th=if.then.$id el=if.else.$id en=if.end.$id
	gen_cond ${ks[0]}
	c=$R
	cbr $c $th $el
	label $th
	gen_node ${ks[1]}
	br $en
	label $el
	if ((${#ks[@]} > 2)); then gen_node ${ks[2]}; else set_status_const 0; fi
	label $en
}
gen_while() {
	local n=$1 id=$((LBL++)) c c2 last ls v cond body ex brk dn
	local -a ks
	ks=(${NK[n]})
	cond=lp.cond.$id body=lp.body.$id ex=lp.exit.$id brk=lp.brk.$id dn=lp.done.$id
	alloca_i32
	last=$R
	emit "store i32 0, ptr %t$last"
	br $cond
	label $cond
	gen_cond ${ks[0]}
	c=$R
	if ((NT[n] == N_UNTIL)); then
		c2=$((TMP++))
		emit "%t$c2 = xor i1 %t$c, true"
		c=$c2
	fi
	cbr $c $body $ex
	label $body
	LBRK[NLOOPS]=$brk LCONT[NLOOPS]=$cond
	((NLOOPS++))
	gen_node ${ks[1]}
	((NLOOPS--))
	get_status
	ls=$R
	emit "store i32 %t$ls, ptr %t$last"
	br $cond
	label $ex
	v=$((TMP++))
	emit "%t$v = load i32, ptr %t$last"
	emit "call void @bash_rt_set_status(i32 %t$v)"
	br $dn
	label $brk
	set_status_const 0
	br $dn
	label $dn
}
gen_for() {
	local n=$1 id=$((LBL++)) b w cnt argv idx last i0 c i1 pp sv ls i2 i3 v head body incr ex brk dn
	head=for.head.$id body=for.body.$id incr=for.incr.$id ex=lp.exit.$id brk=lp.brk.$id dn=lp.done.$id
	new_wb $WB_ARGV
	b=$R
	if ((NFLAG[n])); then
		for w in ${NWS[n]}; do emit_word $b $w $WB_ARGV; done
	else
		mkword
		w=$R
		newpart $PK_VAR @ 1
		add_part $w $R
		emit_word $b $w $WB_ARGV
	fi
	cnt=$((TMP++)) argv=$((TMP++))
	emit "%t$cnt = call i32 @bash_rt_wb_argc(ptr %t$b)"
	emit "%t$argv = call ptr @bash_rt_wb_argv(ptr %t$b)"
	alloca_i32
	idx=$R
	alloca_i32
	last=$R
	emit "store i32 0, ptr %t$idx"
	emit "store i32 0, ptr %t$last"
	br $head
	label $head
	i0=$((TMP++)) c=$((TMP++))
	emit "%t$i0 = load i32, ptr %t$idx"
	emit "%t$c = icmp slt i32 %t$i0, %t$cnt"
	cbr $c $body $ex
	label $body
	i1=$((TMP++)) pp=$((TMP++)) sv=$((TMP++))
	emit "%t$i1 = load i32, ptr %t$idx"
	emit "%t$pp = getelementptr inbounds ptr, ptr %t$argv, i32 %t$i1"
	emit "%t$sv = load ptr, ptr %t$pp"
	vid "${NNAME[n]}"
	emit "call void @bash_rt_set_id(i32 $R, ptr %t$sv, i32 0)"
	LBRK[NLOOPS]=$brk LCONT[NLOOPS]=$incr
	((NLOOPS++))
	first_child $n
	gen_node $R
	((NLOOPS--))
	get_status
	ls=$R
	emit "store i32 %t$ls, ptr %t$last"
	br $incr
	label $incr
	i2=$((TMP++)) i3=$((TMP++))
	emit "%t$i2 = load i32, ptr %t$idx"
	emit "%t$i3 = add i32 %t$i2, 1"
	emit "store i32 %t$i3, ptr %t$idx"
	br $head
	label $ex
	v=$((TMP++))
	emit "%t$v = load i32, ptr %t$last"
	emit "call void @bash_rt_set_status(i32 %t$v)"
	br $dn
	label $brk
	set_status_const 0
	br $dn
	label $dn
	emit "call void @bash_rt_wb_free(ptr %t$b)"
}
# runtime (( )) evaluation: expands $-forms first when the text has them; R = i32 status temp
arith_rt() {   # text word
	local t=$((TMP++)) sv
	if ((${2:-0})); then
		word_to_str $2 $WB_STR
		sv=$R
		emit "%t$t = call i32 @bash_rt_arith(ptr %t$sv)"
		emit "call void @free(ptr %t$sv)"
	else
		S "$1"
		emit "%t$t = call i32 @bash_rt_arith(ptr $R)"
	fi
	R=$t
}
cfor_expr() { arith_native "$1" || arith_rt "$1" $2; }
cfor_cond() {   # R = i1: continue looping?
	local c t
	if arith_native "$1"; then
		c=$((TMP++))
		emit "%t$c = icmp ne i64 $R, 0"
		R=$c
		return
	fi
	arith_rt "$1" $2
	t=$R
	c=$((TMP++))
	emit "%t$c = icmp eq i32 %t$t, 0"
	R=$c
}
gen_cfor() {
	local n=$1 id=$((LBL++)) last c ls v head body incr ex brk dn
	head=cf.head.$id body=cf.body.$id incr=cf.incr.$id ex=lp.exit.$id brk=lp.brk.$id dn=lp.done.$id
	[[ ${NS1[n]} == $NB ]] && cfor_expr "${NS1[n]}" ${NAW0[n]:-0}
	alloca_i32
	last=$R
	emit "store i32 0, ptr %t$last"
	br $head
	label $head
	if [[ ${NS2[n]} == $NB ]]; then
		cfor_cond "${NS2[n]}" ${NAW1[n]:-0}
		cbr $R $body $ex
	else br $body; fi
	label $body
	LBRK[NLOOPS]=$brk LCONT[NLOOPS]=$incr
	((NLOOPS++))
	first_child $n
	gen_node $R
	((NLOOPS--))
	get_status
	ls=$R
	emit "store i32 %t$ls, ptr %t$last"
	br $incr
	label $incr
	[[ ${NS3[n]} == $NB ]] && cfor_expr "${NS3[n]}" ${NAW2[n]:-0}
	br $head
	label $ex
	v=$((TMP++))
	emit "%t$v = load i32, ptr %t$last"
	emit "call void @bash_rt_set_status(i32 %t$v)"
	br $dn
	label $brk
	set_status_const 0
	br $dn
	label $dn
}
gen_case() {
	local n=$1 id=$((LBL++)) w i j it body next f np p m c nx en nci
	local -a cis pats
	en=cs.end.$id
	cis=(${NCIS[n]})
	nci=${#cis[@]}
	w=${NWS[n]# }
	word_to_str ${w%% *} $WB_STR
	w=$R
	for ((i = 0; i < nci; i++)); do
		it=${cis[i]}
		body=cs.body.$((id * 1000 + i))
		if ((i + 1 < nci)); then next=cs.item.$((id * 1000 + i + 1)); else next=cs.none.$id; fi
		if ((i == 0)); then
			f=cs.item.$((id * 1000))
			br $f
			label $f
		fi
		pats=(${CIP[it]})
		np=${#pats[@]}
		for ((j = 0; j < np; j++)); do
			word_to_str ${pats[j]} $WB_PATTERN
			p=$R
			m=$((TMP++)) c=$((TMP++))
			emit "%t$m = call i32 @bash_rt_case_match(ptr %t$w, ptr %t$p)"
			emit "call void @free(ptr %t$p)"
			emit "%t$c = icmp ne i32 %t$m, 0"
			if ((j + 1 < np)); then nx=cs.pat.$((id * 1000 + i * 100 + j + 1)); else nx=$next; fi
			cbr $c $body $nx
			((j + 1 < np)) && label $nx
		done
		label $body
		gen_node ${CIB[it]}
		br $en
		label $next
	done
	if ((nci == 0)); then
		br cs.none.$id
		label cs.none.$id
	fi
	set_status_const 0
	br $en
	label $en
	emit "call void @free(ptr %t$w)"
}
gen_dbl() {
	local n=$1 k arr p0 i prev pat s0 p pn r ra rf rd
	local -a ws strs
	ws=(${NWS[n]})
	k=${#ws[@]}
	arr=$((TMP++))
	FA+="  %t$arr = alloca [$((k + 2)) x ptr]"$'\n'
	p0=$((TMP++))
	emit "%t$p0 = getelementptr inbounds [$((k + 2)) x ptr], ptr %t$arr, i64 0, i64 0"
	S "[["
	emit "store ptr $R, ptr %t$p0"
	for ((i = 0; i < k; i++)); do
		# only the right side of == != =~ is a pattern; every other operand must survive the runtime's unescaping exactly
		pat=0 prev=
		if ((i > 0)) && lit_name ${ws[i - 1]}; then
			prev=$R
			[[ $prev == == || $prev == = || $prev == != || $prev == =~ ]] && pat=1
		fi
		if ((pat)); then
			if [[ $prev == =~ ]]; then word_to_str ${ws[i]} $WB_REGEX; else word_to_str ${ws[i]} $WB_PATTERN; fi
			strs[i]=$R
		else
			word_to_str ${ws[i]} $WB_STR
			s0=$R
			strs[i]=$((TMP++))
			emit "%t${strs[i]} = call ptr @bash_rt_pat_quote(ptr %t$s0)"
		fi
		p=$((TMP++))
		emit "%t$p = getelementptr inbounds [$((k + 2)) x ptr], ptr %t$arr, i64 0, i64 $((i + 1))"
		emit "store ptr %t${strs[i]}, ptr %t$p"
	done
	pn=$((TMP++))
	emit "%t$pn = getelementptr inbounds [$((k + 2)) x ptr], ptr %t$arr, i64 0, i64 $((k + 1))"
	emit "store ptr null, ptr %t$pn"
	tool_index "[["
	USED[R]=1
	redir_begin "${NRS[n]}"
	ra=$RC_A rf=$RC_F rd=$RC_D
	r=$((TMP++))
	emit "%t$r = call i32 @bash_internal_dblbracket(i32 $((k + 1)), ptr %t$arr)"
	emit "call void @bash_rt_set_status(i32 %t$r)"
	redir_end $ra $rf $rd
	for ((i = 0; i < k; i++)); do emit "call void @free(ptr %t${strs[i]})"; done
}
gen_func() {
	local n=$1 id t name di san
	fn_push
	id=$((BLK++))
	emit "call void @bash_rt_fn_enter(i32 %argc, ptr %argv)"
	((IN_FUNC++))
	first_child $n
	gen_node $R
	((IN_FUNC--))
	br Lret
	label Lret
	emit "call void @bash_rt_fn_leave()"
	t=$((TMP++))
	emit "%t$t = call i32 @bash_rt_get_status()"
	emit_term "ret i32 %t$t"
	di=${DNODEIX[n]}
	if [[ -n $di ]]; then name=@bash_ufn_$di
	else
		san=${NNAME[n]}
		san=${san//[!a-zA-Z0-9]/_}
		name=@bash_fn_${id}_$san
		name=${name:0:200}
	fi
	fn_flush "define internal i32 $name(i32 %argc, ptr %argv) "
	fn_pop
	S "${NNAME[n]}"
	emit "call void @bash_rt_defun(ptr $R, ptr $name)"
	[[ -n $di ]] && emit "store i1 true, ptr @bash_ufd_$di"
	set_status_const 0
}
gen_node() {
	local n=$1 ty=${NT[$1]} k any=0 id rhs en c c2 s z v ra rf rd f r t
	local -a ks
	case $ty in
	0) native_test $n || gen_simple $n ;;
	5)
		for k in ${NK[n]}; do gen_node $k; any=1; done
		((any)) || set_status_const 0
		;;
	1) gen_pipe $n ;;
	2 | 3)
		ks=(${NK[n]})
		id=$((LBL++))
		rhs=ao.rhs.$id en=ao.end.$id
		gen_cond ${ks[0]}
		c=$R
		if ((ty == N_OR)); then
			c2=$((TMP++))
			emit "%t$c2 = xor i1 %t$c, true"
			c=$c2
		fi
		cbr $c $rhs $en
		label $rhs
		gen_node ${ks[1]}
		br $en
		label $en
		;;
	4)
		((FCOND++))
		first_child $n
		gen_node $R
		((FCOND--))
		get_status
		s=$R
		z=$((TMP++)) v=$((TMP++))
		emit "%t$z = icmp eq i32 %t$s, 0"
		emit "%t$v = select i1 %t$z, i32 1, i32 0"
		emit "call void @bash_rt_set_status(i32 %t$v)"
		;;
	6 | 11 | 13)
		redir_begin "${NRS[n]}"
		ra=$RC_A rf=$RC_F rd=$RC_D
		if ((ty == N_IF)); then gen_if $n
		elif ((ty == N_CASE)); then gen_case $n
		else first_child $n; gen_node $R; fi
		redir_end $ra $rf $rd
		;;
	7 | 8 | 9 | 10)
		((HOT++))
		redir_begin "${NRS[n]}"
		ra=$RC_A rf=$RC_F rd=$RC_D
		if ((ty == N_FOR)); then gen_for $n
		elif ((ty == N_CFOR)); then gen_cfor $n
		else gen_while $n; fi
		redir_end $ra $rf $rd
		((HOT--))
		;;
	12)
		first_child $n
		gen_block_fn $R
		f=$R
		redir_begin "${NRS[n]}"
		ra=$RC_A rf=$RC_F rd=$RC_D
		r=$((TMP++))
		emit "%t$r = call i32 @bash_rt_subshell(ptr $f)"
		emit "call void @bash_rt_set_status(i32 %t$r)"
		redir_end $ra $rf $rd
		;;
	14)
		((HOT++))
		gen_func $n
		((HOT--))
		;;
	17)
		first_child $n
		gen_block_fn $R
		emit "call void @bash_rt_background(ptr $R)"
		set_status_const 0
		;;
	18)   # time: the runtime notes the clock and CPU times, runs nothing itself, then reports to stderr
		r=$((TMP++))
		emit "%t$r = call i32 @bash_rt_time_start()"
		first_child $n
		gen_node $R
		emit "call void @bash_rt_time_end(i32 %t$r, i32 ${NS1[n]:-0})"
		;;
	15) native_test $n || gen_dbl $n ;;
	16)
		redir_begin "${NRS[n]}"
		ra=$RC_A rf=$RC_F rd=$RC_D
		r=$((TMP++))
		if arith_native "${NS1[n]}"; then
			z=$((TMP++))
			emit "%t$z = icmp ne i64 $R, 0"
			emit "%t$r = select i1 %t$z, i32 0, i32 1"
		else
			arith_rt "${NS1[n]}" ${NAW0[n]:-0}
			t=$R
			emit "%t$r = add i32 %t$t, 0"
		fi
		emit "call void @bash_rt_set_status(i32 %t$r)"
		redir_end $ra $rf $rd
		;;
	esac
	case $ty in 0 | 1 | 12 | 15 | 16) errexit_check ;; esac
	return 0
}
