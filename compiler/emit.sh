# emit.sh - IR emission core: functions, labels, string constants, words, assignments, redirections
#
# The function being generated: FB body, FA allocas, FT terminated, FRET return label, FCOND in-condition depth,
# LOOPBASE (its first entry in the loop stack LBRK/LCONT/NLOOPS).  fn_push/fn_pop nest a new function.
# Finished functions go to FUNCS, string constants to STRS.
declare -A VNI STRI ASSOC DIRECTIX
NVN=0 NSTR=0 TMP=0 LBL=0 BLK=0 FD=0 NLOOPS=0 COLD_OK=0 HOT=0 IN_FUNC=0 ND=0
FB= FA= FT=0 FRET=Lret FCOND=0 LOOPBASE=0
FUNCS=() STRS=()

vid() {
	R=${VNI[_$1]}
	[[ -n $R ]] && return
	R=$NVN
	VNI[_$1]=$R
	VN[NVN]=$1
	((NVN++))
}
is_ident() { [[ $1 == [a-zA-Z_]* && $1 != *[!a-zA-Z0-9_]* ]]; }
special_kind() {   # R = SP_* for special parameters, 0 otherwise; status 0 when special
	case $1 in
	'?') R=1 ;; '#') R=2 ;; '$') R=3 ;; '!') R=4 ;; '-') R=5 ;; 0) R=6 ;; '@') R=10 ;; '*') R=11 ;;
	RANDOM) R=7 ;; PPID) R=8 ;; SECONDS) R=9 ;; FUNCNAME) R=12 ;; BASH_SOURCE) R=13 ;;
	EPOCHSECONDS) R=14 ;; EPOCHREALTIME) R=15 ;;
	*) R=0; return 1 ;;
	esac
	return 0
}
# Cold code: top-level script code outside loops and functions runs once, so runtime calls there are marked
# noinline: inlining the runtime helpers (rtlink.sh) would only make the script bigger and slower to compile.
emit() {
	local r pre
	if ((FT)); then FB+="dead$((LBL++)):"$'\n'; FT=0; fi
	if ((COLD_OK && !HOT)) && [[ $1 == *"call "*')' ]]; then
		r=${1#*"call "}
		pre=${r%%@*}
		if [[ ${r:${#pre}:9} == @bash_rt_ ]]; then FB+="  $1 noinline"$'\n'; return; fi
	fi
	FB+="  $1"$'\n'
}
emit_term() {
	if ((FT)); then FB+="dead$((LBL++)):"$'\n'; fi
	FB+="  $1"$'\n'
	FT=1
}
label() {
	((FT)) || emit_term "br label %$1"
	FB+="$1:"$'\n'
	FT=0
}
br() { emit_term "br label %$1"; }
cbr() { emit_term "br i1 %t$1, label %$2, label %$3"; }

S() {   # R = global string constant for $1
	local k=_$1 id
	id=${STRI[$k]}
	if [[ -z $id ]]; then
		id=$NSTR
		STRI[$k]=$id
		((NSTR++))
		irbytes "$1"
		STRS+=("@.str.$id = private unnamed_addr constant [$((${#1} + 1)) x i8] c\"$R\\00\", align 1")
	fi
	R=@.str.$id
}
is_user_fn() { [[ -n ${USERFN[_$1]} ]]; }
# names declared with declare/local/typeset -A anywhere in the script: their subscripts are string keys
is_assoc() { [[ -n ${ASSOC[_$1]} ]]; }
add_assoc() { ASSOC[_$1]=1; }
# "base[sub]" with base an assoc name and sub not @ / *: AS_BASE, AS_SUB
assoc_sub() {
	local t=$1
	[[ $t == *']' ]] || return 1
	AS_BASE=${t%%'['*}
	[[ $AS_BASE != "$t" && -n $AS_BASE ]] || return 1
	is_assoc "$AS_BASE" || return 1
	AS_SUB=${t#*'['}
	AS_SUB=${AS_SUB%']'}
	[[ $AS_SUB != @ && $AS_SUB != '*' ]]
}
key_word() {   # assoc subscript: expanded like a double-quoted word (no split/glob) -> R = word
	local w
	sub_enter "$1"
	mkword
	w=$R
	scan_parts $w "" 0
	sub_leave
	R=$w
}
key_tmp() {   # R = tmp holding the expanded key, owned by the runtime's key ring
	local t r
	key_word "$1"
	word_to_str $R $WB_STR
	t=$R
	r=$((TMP++))
	emit "%t$r = call ptr @bash_rt_keytmp(ptr %t$t)"
	R=$r
}
new_wb() {
	local t=$((TMP++))
	emit "%t$t = call ptr @bash_rt_wb_new(i32 $1)"
	R=$t
}

fn_push() {
	FSB[FD]=$FB FSA[FD]=$FA FST[FD]=$FT FSR[FD]=$FRET FSC[FD]=$FCOND FSL[FD]=$LOOPBASE
	((FD++))
	FB= FA= FT=0 FRET=Lret FCOND=0 LOOPBASE=$NLOOPS
}
fn_pop() {
	((FD--))
	FB=${FSB[FD]} FA=${FSA[FD]} FT=${FST[FD]} FRET=${FSR[FD]} FCOND=${FSC[FD]} LOOPBASE=${FSL[FD]}
	FSB[FD]= FSA[FD]=
}
fn_flush() { FUNCS+=("$1{"$'\n'"entry:"$'\n'"$FA  br label %start"$'\n'"start:"$'\n'"$FB}"$'\n'$'\n'); }
gen_block_fn() {   # body -> R = i32 @bash_blk_N()
	local id=$((BLK++)) t
	fn_push
	gen_node $1
	br Lret
	label Lret
	t=$((TMP++))
	emit "%t$t = call i32 @bash_rt_get_status()"
	emit_term "ret i32 %t$t"
	fn_flush "define internal i32 @bash_blk_$id() "
	fn_pop
	R=@bash_blk_$id
}
gen_str_fn() {   # word mode -> R = ptr @bash_str_N() returning a malloc'd string; "null" if no word
	local id b t
	if ((!${1:-0})); then R=null; return; fi
	id=$((BLK++))
	fn_push
	new_wb $2
	b=$R
	emit_word $b $1 $2
	t=$((TMP++))
	emit "%t$t = call ptr @bash_rt_wb_str(ptr %t$b)"
	emit_term "ret ptr %t$t"
	fn_flush "define internal ptr @bash_str_$id() "
	fn_pop
	R=@bash_str_$id
}
word_const() {   # R = text of a word made of literal parts only (unquoted ones without glob characters)
	local p t s=
	((WN[$1])) || return 1
	for p in ${WP[$1]}; do
		((PK[p] == 0)) || return 1
		t=${PT[p]}
		if ((!PQ[p])) && [[ $t == *'*'* || $t == *'?'* || $t == *'['* ]]; then return 1; fi
		s+=$t
	done
	R=$s
}
opd_const() {   # word pat -> R: operand that is a plain literal, no runtime evaluation needed
	local p t s=
	for p in ${WP[$1]}; do
		((PK[p] == 0)) || return 1
		t=${PT[p]}
		if (($2)) && [[ $t == *'*'* || $t == *'?'* || $t == *'['* || $t == *']'* || $t == *'\'* ]]; then return 1; fi
		s+=$t
	done
	R=$s
}
# operands of ${x OP a / c}: OPA / OPC (function or null), SCA / SCC set by param_consts
param_operands() {   # part pat
	local p=$1
	if ((PA[p])) && opd_const ${PA[p]} $2; then HCA=1 CA_=$R OPA=null
	else
		HCA=0
		if (($2)); then gen_str_fn "${PA[p]}" $WB_PATTERN; else gen_str_fn "${PA[p]}" $WB_STR; fi
		OPA=$R
	fi
	if ((PC[p])) && opd_const ${PC[p]} 0; then HCC=1 CC_=$R OPC=null
	else
		HCC=0
		gen_str_fn "${PC[p]}" $WB_STR
		OPC=$R
	fi
}
param_consts() {
	if ((HCA)); then S "$CA_"; SCA=$R; else SCA=null; fi
	if ((HCC)); then S "$CC_"; SCC=$R; else SCC=null; fi
}
emit_word() {   # wb word mode
	local b=$1 w=$2 mode=$3 p nm s
	if ((mode == 0)) && word_const $w; then
		S "$R"
		emit "call void @bash_rt_wb_const(ptr %t$b, ptr $R)"
		return
	fi
	for p in ${WP[w]}; do
		case ${PK[p]} in
		0)
			S "${PT[p]}"
			if ((PQ[p])); then emit "call void @bash_rt_wb_lit(ptr %t$b, ptr $R, i32 1)"
			else emit "call void @bash_rt_wb_lit(ptr %t$b, ptr $R, i32 2)"; fi
			;;
		1)
			nm=${PT[p]}
			if special_kind "$nm"; then emit "call void @bash_rt_wb_special(ptr %t$b, i32 $R, i32 ${PQ[p]})"
			elif [[ $nm == [0-9]* ]]; then c_atoi "$nm"; emit "call void @bash_rt_wb_pos(ptr %t$b, i32 $R, i32 ${PQ[p]})"
			else vid "$nm"; emit "call void @bash_rt_wb_var_id(ptr %t$b, i32 $R, i32 ${PQ[p]})"; fi
			;;
		2) emit_param $b $p ;;
		3)
			gen_block_fn ${PB[p]}
			emit "call void @bash_rt_wb_cmdsub(ptr %t$b, ptr $R, i32 ${PQ[p]})"
			;;
		5)
			gen_block_fn ${PB[p]}
			emit "call void @bash_rt_wb_procsub(ptr %t$b, ptr $R)"
			;;
		4)
			if arith_native "${PT[p]}"; then emit "call void @bash_rt_wb_int(ptr %t$b, i64 $R, i32 ${PQ[p]})"
			elif ((PA[p])); then
				word_to_str ${PA[p]} $WB_STR
				s=$R
				emit "call void @bash_rt_wb_arith(ptr %t$b, ptr %t$s, i32 ${PQ[p]})"
				emit "call void @free(ptr %t$s)"
			else
				S "${PT[p]}"
				emit "call void @bash_rt_wb_arith(ptr %t$b, ptr $R, i32 ${PQ[p]})"
			fi
			;;
		esac
	done
	((mode == 0)) && emit "call void @bash_rt_wb_end(ptr %t$b)"
}
emit_param() {   # wb part (PK_PARAM)
	local b=$1 p=$2 op=${PO[$2]} text=${PT[$2]} q=${PQ[$2]} pat=0 abase asub k ca d i ch colon offs lens haslen ov lv mark term base ix iv sub id fa fc
	((op >= P_TRIM_S && op <= P_SUBST_SUF)) && pat=1
	if ((op != P_KEYS && op != P_INDIRECT)) && assoc_sub "$text"; then
		abase=$AS_BASE asub=$AS_SUB
		key_tmp "$asub"
		k=$R
		param_operands $p $pat
		vid "$abase"
		id=$R
		param_consts
		emit "call void @bash_rt_wb_aparam(ptr %t$b, i32 $id, ptr %t$k, i32 $op, ptr $OPA, ptr $OPC, ptr $SCA, ptr $SCC, i32 $q)"
		return
	fi
	if ((op == P_SUBSTR && PA[p])) && is_ident "$text" && ! special_kind "$text" && opd_const ${PA[p]} 0 && [[ $R != *'?'* ]]; then
		ca=$R d=0 colon=-1
		for ((i = 0; i < ${#ca}; i++)); do
			ch=${ca:i:1}
			if [[ $ch == '(' ]]; then ((d++))
			elif [[ $ch == ')' ]]; then ((d--))
			elif [[ $ch == : ]] && ((d == 0)); then colon=$i; break; fi
		done
		haslen=0 lv=0
		if ((colon >= 0)); then offs=${ca:0:colon} lens=${ca:colon+1} haslen=1; else offs=$ca lens=; fi
		mark=${#FB} term=$FT
		if [[ $offs == $NB ]] && arith_native "$offs"; then
			ov=$R
			if ((!haslen)) || { [[ $lens == $NB ]] && arith_native "$lens"; }; then
				((haslen)) && lv=$R
				vid "$text"
				emit "call void @bash_rt_wb_substr_id(ptr %t$b, i32 $R, i64 $ov, i64 $lv, i32 $haslen, i32 $q)"
				return
			fi
		fi
		FB=${FB:0:mark} FT=$term
	fi
	if ((op != P_KEYS && op != P_INDIRECT && op != P_ASSIGN && op != P_ASSIGN_NC)) && [[ $text == *']' ]]; then   # ${a[expr]} ${a[@]} ${a[*]} of an indexed array
		base=${text%%'['*}
		if [[ $base != "$text" && -n $base ]]; then
			ix=${text#*'['}
			ix=${ix%']'}
			iv=0 sub=0
			if is_ident "$base" && ! special_kind "$base" && ! is_assoc "$base"; then
				mark=${#FB} term=$FT
				if [[ $ix == @ ]]; then sub=1
				elif [[ $ix == '*' ]]; then sub=2
				elif [[ $ix == $NB ]] && arith_native "$ix"; then sub=3 iv=$R
				else FB=${FB:0:mark} FT=$term; fi
				if ((sub)); then
					param_operands $p $pat
					vid "$base"
					id=$R
					param_consts
					emit "call void @bash_rt_wb_param_id(ptr %t$b, i32 $id, i32 $sub, i64 $iv, i32 $op, ptr $OPA, ptr $OPC, ptr $SCA, ptr $SCC, i32 $q)"
					return
				fi
			fi
		fi
	fi
	if is_ident "$text" && ! special_kind "$text"; then
		param_operands $p $pat
		vid "$text"
		id=$R
		param_consts
		emit "call void @bash_rt_wb_param_id(ptr %t$b, i32 $id, i32 0, i64 0, i32 $op, ptr $OPA, ptr $OPC, ptr $SCA, ptr $SCC, i32 $q)"
		return
	fi
	if ((pat)); then gen_str_fn "${PA[p]}" $WB_PATTERN; else gen_str_fn "${PA[p]}" $WB_STR; fi
	fa=$R
	gen_str_fn "${PC[p]}" $WB_STR
	fc=$R
	S "$text"
	emit "call void @bash_rt_wb_param(ptr %t$b, ptr $R, i32 $op, ptr $fa, ptr $fc, i32 $q)"
}
word_to_str() {   # word mode -> R = tmp holding a malloc'd string
	local b s
	new_wb $2
	b=$R
	((${1:-0})) && emit_word $b $1 $2
	s=$((TMP++))
	emit "%t$s = call ptr @bash_rt_wb_str(ptr %t$b)"
	R=$s
}
# assignment value that needs no word builder: a constant, $var, $N or a special.  R = an IR operand (borrowed
# string); for a plain $var with copy_dst >= 0 emits a direct copy and sets R empty.
word_borrow() {   # word copy_dst
	local w=$1 dst=$2 p lit=1 s= tx base ix iv t nm mark term
	if ((!w || !WN[w])); then S ""; return 0; fi
	for p in ${WP[w]}; do ((PK[p])) && lit=0; done
	if ((lit)); then
		for p in ${WP[w]}; do s+=${PT[p]}; done
		S "$s"
		return 0
	fi
	p=${WL[w]}
	if ((WN[w] == 1 && PK[p] == 2 && PO[p] == 0)); then   # ${a[expr]} of an indexed array
		tx=${PT[p]}
		[[ $tx == *']' ]] || return 1
		base=${tx%%'['*}
		[[ $base != "$tx" && -n $base ]] || return 1
		ix=${tx#*'['}
		ix=${ix%']'}
		if ! is_ident "$base" || special_kind "$base" || is_assoc "$base" || [[ $ix != $NB || $ix == @ || $ix == '*' ]]; then return 1; fi
		mark=${#FB} term=$FT
		if ! arith_native "$ix"; then FB=${FB:0:mark} FT=$term; return 1; fi
		iv=$R
		t=$((TMP++))
		vid "$base"
		emit "%t$t = call ptr @bash_rt_gets_elem_e(i32 $R, i64 $iv)"
		R=%t$t
		return 0
	fi
	((WN[w] == 1 && PK[p] == 1)) || return 1
	nm=${PT[p]}
	if special_kind "$nm"; then
		((R == 10 || R == 11 || R == 12 || R == 13)) && return 1
		t=$((TMP++))
		emit "%t$t = call ptr @bash_rt_special_s(i32 $R)"
	elif [[ $nm == [0-9]* ]]; then
		t=$((TMP++))
		c_atoi "$nm"
		emit "%t$t = call ptr @bash_rt_pos_u(i32 $R)"
	elif ((dst >= 0)); then
		vid "$nm"
		emit "call void @bash_rt_copy_id(i32 $dst, i32 $R)"
		R=
		return 0
	else
		t=$((TMP++))
		vid "$nm"
		emit "%t$t = call ptr @bash_rt_gets_e(i32 $R)"
	fi
	R=%t$t
	return 0
}
set_status_const() { emit "call void @bash_rt_set_status(i32 $1)"; }
get_status() {
	local t=$((TMP++))
	emit "%t$t = call i32 @bash_rt_get_status()"
	R=$t
}
alloca_i32() {
	local t=$((TMP++))
	FA+="  %t$t = alloca i32"$'\n'
	R=$t
}

# [key]=value array-literal element: split at the first unquoted "]=" -> K_W, V_W
split_kv() {
	local w=$1 p0 i j p t from before after k v c np
	local -a ps
	ps=(${WP[w]})
	np=${#ps[@]}
	((np)) || return 1
	p0=${ps[0]}
	t=${PT[p0]}
	((PK[p0] == 0 && !PQ[p0])) && [[ $t == '['* ]] || return 1
	for ((i = 0; i < np; i++)); do
		p=${ps[i]}
		((PK[p] != 0 || PQ[p])) && continue
		t=${PT[p]}
		if ((i == 0)); then from=${t:1}; else from=$t; fi
		[[ $from == *']='* ]] || continue
		before=${from%%']='*}
		after=${from#*']='}
		mkword
		k=$R
		mkword
		v=$R
		for ((j = 0; j < i; j++)); do
			part_copy ${ps[j]}
			c=$R
			if ((j == 0)); then t=${PT[c]}; PT[c]=${t:1}; fi
			((PK[c] == 0)) && PQ[c]=1
			add_part $k $c
		done
		[[ -n $before ]] && add_lit $k "$before" 1 0
		[[ -n $after ]] && add_lit $v "$after" 0 0
		for ((j = i + 1; j < np; j++)); do add_part $v ${ps[j]}; done
		((WN[v])) || add_lit $v "" 1 1
		((WN[k])) || add_lit $k "" 1 1
		K_W=$k V_W=$v
		return 0
	done
	return 1
}
gen_assign() {   # assignment tmp
	local a=$1 tmp=$2 name=${AN[$1]} app=${AAPP[$1]:-0} val=${AVAL[$1]:-0} keyed=0 e id b c av ks vs sv s base ix iv v bop dst kw vw i ne lp op
	local -a els
	if ((AISARR[a])); then for e in ${AEL[a]}; do split_kv $e && keyed=1; done; fi
	if ((AISARR[a])) && { ((keyed)) || is_assoc "$name"; }; then   # begin / kv / push / end
		vid "$name"
		id=$R
		emit "call void @bash_rt_arr_begin(i32 $id, i32 $app)"
		els=(${AEL[a]})
		ne=${#els[@]}
		i=0
		while ((i < ne)); do
			if split_kv ${els[i]}; then
				kw=$K_W vw=$V_W
				word_to_str $kw $WB_STR
				ks=$R
				word_to_str $vw $WB_STR
				vs=$R
				emit "call void @bash_rt_arr_kv(i32 $id, ptr %t$ks, ptr %t$vs, i32 0)"
				emit "call void @free(ptr %t$ks)"
				emit "call void @free(ptr %t$vs)"
				((i++))
				continue
			fi
			new_wb $WB_ARGV
			b=$R
			while ((i < ne)) && ! split_kv ${els[i]}; do
				emit_word $b ${els[i]} $WB_ARGV
				((i++))
			done
			c=$((TMP++)) av=$((TMP++))
			emit "%t$c = call i32 @bash_rt_wb_argc(ptr %t$b)"
			emit "%t$av = call ptr @bash_rt_wb_argv(ptr %t$b)"
			emit "call void @bash_rt_arr_push(i32 $id, i32 %t$c, ptr %t$av)"
			emit "call void @bash_rt_wb_free(ptr %t$b)"
		done
		emit "call void @bash_rt_arr_end(i32 $id)"
		return
	fi
	if ((!tmp && !AISARR[a])) && assoc_sub "$name"; then   # h[key]=value
		base=$AS_BASE
		key_word "$AS_SUB"
		word_to_str $R $WB_STR
		ks=$R
		word_to_str $val $WB_STR
		sv=$R
		vid "$base"
		emit "call void @bash_rt_aset(i32 $R, ptr %t$ks, ptr %t$sv, i32 $app)"
		emit "call void @free(ptr %t$ks)"
		emit "call void @free(ptr %t$sv)"
		return
	fi
	if ((AISARR[a])); then
		new_wb $WB_ARGV
		b=$R
		for e in ${AEL[a]}; do emit_word $b $e $WB_ARGV; done
		c=$((TMP++)) v=$((TMP++))
		emit "%t$c = call i32 @bash_rt_wb_argc(ptr %t$b)"
		emit "%t$v = call ptr @bash_rt_wb_argv(ptr %t$b)"
		vid "$name"
		emit "call void @bash_rt_set_array_id(i32 $R, i32 %t$c, ptr %t$v, i32 $app)"
		emit "call void @bash_rt_wb_free(ptr %t$b)"
		return
	fi
	lp=${WL[val]}
	if ((!tmp && !app && val && WN[val] == 1 && PK[lp] == 4)) && is_ident "$name"; then
		if arith_native "${PT[lp]}"; then
			v=$R
			vid "$name"
			emit "call void @bash_rt_seti(i32 $R, i64 $v)"
			return
		fi
	fi
	if ((!tmp)) && ! is_ident "$name" && [[ $name == *']' && $name == *'['* ]]; then   # name[expr]=value, compile-time index
		base=${name%%'['*}
		ix=${name#*'['}
		ix=${ix%']'}
		if is_ident "$base" && ! special_kind "$base" && [[ $ix != @ && $ix != '*' ]] && arith_native "$ix"; then
			iv=$R
			if ((!app && val && WN[val] == 1 && PK[lp] == 4)) && arith_native "${PT[lp]}"; then
				v=$R
				vid "$base"
				emit "call void @bash_rt_seti_elem(i32 $R, i64 $iv, i64 $v)"
				return
			fi
			if word_borrow $val -1; then
				bop=$R
				vid "$base"
				emit "call void @bash_rt_set_elem_id(i32 $R, i64 $iv, ptr $bop, i32 $app)"
				return
			fi
			word_to_str $val $WB_STR
			sv=$R
			vid "$base"
			emit "call void @bash_rt_set_elem_id(i32 $R, i64 $iv, ptr %t$sv, i32 $app)"
			emit "call void @free(ptr %t$sv)"
			return
		fi
	fi
	if ((!tmp)) && ! is_ident "$name" && [[ $name == *']' && $name == *'['* ]]; then   # name[sub]=value, sub expanded at run time
		base=${name%%'['*}
		ix=${name#*'['}
		ix=${ix%']'}
		if is_ident "$base" && ! special_kind "$base" && [[ $ix != @ && $ix != '*' ]]; then
			key_word "$ix"
			word_to_str $R $WB_STR
			ks=$R
			word_to_str $val $WB_STR
			sv=$R
			vid "$base"
			emit "call void @bash_rt_aset(i32 $R, ptr %t$ks, ptr %t$sv, i32 $app)"
			emit "call void @free(ptr %t$ks)"
			emit "call void @free(ptr %t$sv)"
			return
		fi
	fi
	if is_ident "$name"; then   # x=const / x=$y / x=$1: no temporary string
		dst=-1
		if ((!tmp && !app)); then vid "$name"; dst=$R; fi
		if word_borrow $val $dst; then
			[[ -z $R ]] && return
			op=$R
			vid "$name"
			if ((tmp)); then emit "call void @bash_rt_tmp_set_id(i32 $R, ptr $op)"
			else emit "call void @bash_rt_set_id(i32 $R, ptr $op, i32 $app)"; fi
			return
		fi
	fi
	word_to_str $val $WB_STR
	s=$R
	if ! is_ident "$name"; then
		S "$name"
		emit "call void @bash_rt_set(ptr $R, ptr %t$s, i32 $app)"
	elif ((tmp)); then
		vid "$name"
		emit "call void @bash_rt_tmp_set_id(i32 $R, ptr %t$s)"
	else
		vid "$name"
		emit "call void @bash_rt_set_id(i32 $R, ptr %t$s, i32 $app)"
	fi
	emit "call void @free(ptr %t$s)"
}

# redirections: RC_A (active), RC_F / RC_D (fail / done labels) for redir_end
redir_begin() {   # " redirection ids"
	local id r s t z ok
	RC_A=0
	[[ -n $1 ]] || return 0
	id=$((LBL++))
	ok=rd.ok.$id
	emit "call void @bash_rt_redir_push()"
	for r in $1; do
		word_to_str ${RT[r]} $WB_STR
		s=$R
		emit "call void @bash_rt_redir(i32 ${RFD[r]}, i32 ${ROP[r]}, ptr %t$s)"
		emit "call void @free(ptr %t$s)"
	done
	t=$((TMP++)) z=$((TMP++))
	emit "%t$t = call i32 @bash_rt_redir_ok()"
	emit "%t$z = icmp ne i32 %t$t, 0"
	cbr $z $ok rd.fail.$id
	label $ok
	RC_A=1 RC_F=rd.fail.$id RC_D=rd.done.$id
}
redir_end() {   # active fail done
	(($1)) || return 0
	br $3
	label $2
	set_status_const 1
	label $3
	emit "call void @bash_rt_redir_pop()"
}
