# arith.sh - native arithmetic
# Arithmetic that mentions no $-expansions is parsed here, at compile time, and turned into straight-line i64
# code; variables are read/written through their slot ids.  Anything unusual falls back to the runtime evaluator
# (which also reports syntax errors at run time like bash does).
#
# Parser state: AT text, AP position, AE error (saved/restored by arith_native, which can nest through key_tmp).
# Lvalues (ac_lvalue): LV_ID, LV_EL (indexed element), LV_SP (special), LV_IX (index value), LV_KEY (assoc key tmp, -1)
AT= AP=0 AE=0

alloca_i64() {
	local t=$((TMP++))
	FA+="  %t$t = alloca i64"$'\n'
	R=$t
}
ac_ws() { while [[ ${AT:AP:1} == $WSP ]]; do ((AP++)); done; }
ac_subscript_end() {   # index just after '[' -> R = index of the matching ']', -1 if none
	local i=$1 d=1 q= c
	while c=${AT:i:1}; [[ -n $c ]]; do
		if [[ -n $q ]]; then
			if [[ $c == "$q" ]]; then q=
			elif [[ $c == '\' && $q == '"' && -n ${AT:i+1:1} ]]; then ((i++)); fi
			((i++))
			continue
		fi
		if [[ $c == "'" || $c == '"' ]]; then q=$c
		elif [[ $c == '[' ]]; then ((d++))
		elif [[ $c == ']' ]]; then
			if ((--d == 0)); then R=$i; return 0; fi
		fi
		((i++))
	done
	R=-1
	return 1
}
ac_lvalue() {   # status 0 when an lvalue was parsed (AE may still be set)
	local s st name sp id e sub key idx
	ac_ws
	s=$AP
	[[ ${AT:s:1} == [a-zA-Z_] ]] || return 1
	st=$s
	while [[ ${AT:s:1} == [a-zA-Z0-9_] ]]; do ((s++)); done
	name=${AT:st:s-st}
	special_kind "$name"
	sp=$R
	((sp >= 10)) && return 1
	if [[ ${AT:s:1} == '[' ]] && ((!sp)) && is_assoc "$name"; then
		ac_subscript_end $((s + 1))
		e=$R
		if ((e < 0)); then AE=1 LV_ID=0 LV_EL=0 LV_SP=$sp LV_IX= LV_KEY=-1; return 0; fi
		sub=${AT:s+1:e-s-1}
		vid "$name"
		id=$R
		key_tmp "$sub"
		key=$R
		AP=$((e + 1))
		LV_ID=$id LV_EL=0 LV_SP=$sp LV_IX= LV_KEY=$key
		return 0
	fi
	id=0
	if ((!sp)); then vid "$name"; id=$R; fi
	AP=$s
	if [[ ${AT:s:1} == '[' ]]; then
		((AP++))
		ac_assign
		idx=$R
		ac_ws
		if [[ ${AT:AP:1} != ']' ]]; then AE=1 LV_ID=$id LV_EL=0 LV_SP=$sp LV_IX= LV_KEY=-1; return 0; fi
		((AP++))
		LV_ID=$id LV_EL=1 LV_SP=$sp LV_IX=$idx LV_KEY=-1
		return 0
	fi
	LV_ID=$id LV_EL=0 LV_SP=$sp LV_IX= LV_KEY=-1
	return 0
}
lv_load() {   # id el sp ix key -> R
	local r=%t$((TMP++))
	if (($3)); then emit "$r = call i64 @bash_rt_special_i(i32 $3)"
	elif (($5 >= 0)); then emit "$r = call i64 @bash_rt_ageti(i32 $1, ptr %t$5)"
	elif (($2)); then emit "$r = call i64 @bash_rt_geti_elem(i32 $1, i64 $4)"
	else emit "$r = call i64 @bash_rt_geti(i32 $1)"; fi
	R=$r
}
lv_store() {   # id el sp ix key value
	if (($3)); then AE=1; return; fi
	if (($5 >= 0)); then emit "call void @bash_rt_aseti(i32 $1, ptr %t$5, i64 $6)"
	elif (($2)); then emit "call void @bash_rt_seti_elem(i32 $1, i64 $4, i64 $6)"
	else emit "call void @bash_rt_seti(i32 $1, i64 $6)"; fi
}
ac_bin() { local t=%t$((TMP++)); emit "$t = $1 i64 $2, $3"; R=$t; }
ac_cmp() {
	local c=%t$((TMP++)) t=%t$((TMP++))
	emit "$c = icmp $1 i64 $2, $3"
	emit "$t = zext i1 $c to i64"
	R=$t
}
ac_dollar() {   # $name ${name} ${#name} ${#arr[@]} ${name[expr]} $N $# $? inside arithmetic
	local s=$((AP + 1)) brace=0 len=0 r c n d st name id e sub k ix r2
	if [[ ${AT:s:1} == '{' ]]; then
		brace=1
		((s++))
		if [[ ${AT:s:1} == '#' && ${AT:s+1:1} != '}' ]]; then len=1; ((s++)); fi
	fi
	r=%t$((TMP++))
	c=${AT:s:1}
	if ((!brace)) && [[ $c == '#' || $c == '?' ]]; then
		if [[ $c == '#' ]]; then emit "$r = call i64 @bash_rt_special_i(i32 2)"; else emit "$r = call i64 @bash_rt_special_i(i32 1)"; fi
		AP=$((s + 1)) R=$r
		return
	fi
	if [[ $c == [0-9] ]]; then
		n=0
		if ((brace)); then
			while d=${AT:s:1}; [[ $d == [0-9] ]]; do n=$((n * 10 + d)); ((s++)); done
		else
			n=$((c))
			((s++))
		fi
		if ((brace)); then
			if [[ ${AT:s:1} != '}' ]]; then AE=1 R=$r; return; fi
			((s++))
		fi
		if ((len || n == 0)); then AE=1 R=$r; return; fi
		emit "$r = call i64 @bash_rt_pos_i(i32 $n)"
		AP=$s R=$r
		return
	fi
	if [[ $c != [a-zA-Z_] ]]; then AE=1 R=$r; return; fi
	st=$s
	while [[ ${AT:s:1} == [a-zA-Z0-9_] ]]; do ((s++)); done
	name=${AT:st:s-st}
	if special_kind "$name"; then AE=1 R=$r; return; fi
	vid "$name"
	id=$R
	if ((!brace)); then
		emit "$r = call i64 @bash_rt_geti(i32 $id)"
		AP=$s R=$r
		return
	fi
	c=${AT:s:1}
	if [[ $c == '[' ]] && is_assoc "$name" && ((!len)) && ! [[ ${AT:s+1:1} == [@*] && ${AT:s+2:1} == ']' ]]; then
		ac_subscript_end $((s + 1))
		e=$R
		if ((e < 0)) || [[ ${AT:e+1:1} != '}' ]]; then AE=1 R=$r; return; fi
		sub=${AT:s+1:e-s-1}
		key_tmp "$sub"
		k=$R
		emit "$r = call i64 @bash_rt_ageti(i32 $id, ptr %t$k)"
		AP=$((e + 2)) R=$r
		return
	fi
	if [[ $c == '[' ]]; then
		if [[ ${AT:s+1:1} == [@*] && ${AT:s+2:2} == ']}' ]] && ((len)); then
			emit "$r = call i64 @bash_rt_arr_len(i32 $id)"
			AP=$((s + 4)) R=$r
			return
		fi
		if ((len)); then AE=1 R=$r; return; fi
		AP=$((s + 1))
		ac_assign
		ix=$R
		ac_ws
		if [[ ${AT:AP:2} != ']}' ]]; then AE=1 R=$r; return; fi
		((AP += 2))
		r2=%t$((TMP++))
		emit "$r2 = call i64 @bash_rt_geti_elem(i32 $id, i64 $ix)"
		R=$r2
		return
	fi
	if [[ $c != '}' ]]; then AE=1 R=$r; return; fi
	if ((len)); then emit "$r = call i64 @bash_rt_strlen_id(i32 $id)"; else emit "$r = call i64 @bash_rt_geti(i32 $id)"; fi
	AP=$((s + 1)) R=$r
}
ac_primary() {
	local c h base v n id el sp ix key
	ac_ws
	c=${AT:AP:1}
	if [[ $c == '$' ]]; then ac_dollar; return; fi
	if [[ $c == '(' ]]; then
		((AP++))
		ac_comma
		v=$R
		ac_ws
		if [[ ${AT:AP:1} == ')' ]]; then ((AP++)); else AE=1; fi
		R=$v
		return
	fi
	if [[ $c == [0-9] ]]; then
		h=$AP
		while [[ ${AT:h:1} == [0-9] ]]; do ((h++)); done
		if [[ ${AT:h:1} == '#' ]]; then
			c_atoi "${AT:AP:h-AP}"
			base=$R
			AP=$((h + 1))
			((base > 1 && base <= 36)) || base=10
			c_strtol "${AT:AP}" $base
		else
			c_strtol "${AT:AP}" 0
		fi
		if ((SL == 0)); then AE=1 R=0; return; fi
		((AP += SL))
		return
	fi
	if ! ac_lvalue; then AE=1 R=0; return; fi
	id=$LV_ID el=$LV_EL sp=$LV_SP ix=$LV_IX key=$LV_KEY
	lv_load "$id" $el $sp "$ix" $key
	v=$R
	ac_ws
	if [[ ${AT:AP:2} == '++' || ${AT:AP:2} == '--' ]]; then
		if [[ ${AT:AP:1} == + ]]; then ac_bin add $v 1; else ac_bin sub $v 1; fi
		lv_store "$id" $el $sp "$ix" $key $R
		((AP += 2))
	fi
	R=$v
}
ac_unary() {
	local c v n id el sp ix key
	ac_ws
	c=${AT:AP:1}
	if [[ $c == [+-] && ${AT:AP+1:1} == "$c" ]]; then
		((AP += 2))
		if ! ac_lvalue; then AE=1 R=0; return; fi
		id=$LV_ID el=$LV_EL sp=$LV_SP ix=$LV_IX key=$LV_KEY
		lv_load "$id" $el $sp "$ix" $key
		v=$R
		if [[ $c == + ]]; then ac_bin add $v 1; else ac_bin sub $v 1; fi
		n=$R
		lv_store "$id" $el $sp "$ix" $key $n
		R=$n
		return
	fi
	case $c in
	'!') ((AP++)); ac_unary; ac_cmp eq $R 0 ;;
	'~') ((AP++)); ac_unary; ac_bin xor $R -1 ;;
	'-') ((AP++)); ac_unary; ac_bin sub 0 $R ;;
	'+') ((AP++)); ac_unary ;;
	*) ac_primary ;;
	esac
}
ac_pow() {
	local l r t
	ac_unary
	l=$R
	ac_ws
	if [[ ${AT:AP:2} == '**' ]]; then
		((AP += 2))
		ac_pow
		r=$R
		t=%t$((TMP++))
		emit "$t = call i64 @bash_rt_pow(i64 $l, i64 $r)"
		R=$t
		return
	fi
	R=$l
}
ac_mul() {
	local l r c c1 t
	ac_pow
	l=$R
	while :; do
		ac_ws
		c=${AT:AP:1} c1=${AT:AP+1:1}
		if [[ $c == '*' && $c1 != '*' && $c1 != = ]] || [[ ($c == / || $c == %) && $c1 != = ]]; then
			((AP++))
			ac_pow
			r=$R
			if [[ $c == '*' ]]; then ac_bin mul $l $r; l=$R
			else
				t=%t$((TMP++))
				if [[ $c == / ]]; then emit "$t = call i64 @bash_rt_div(i64 $l, i64 $r)"; else emit "$t = call i64 @bash_rt_mod(i64 $l, i64 $r)"; fi
				l=$t
			fi
		else R=$l; return; fi
	done
}
ac_add() {
	local l c c1
	ac_mul
	l=$R
	while :; do
		ac_ws
		c=${AT:AP:1} c1=${AT:AP+1:1}
		if [[ $c == [+-] && $c1 != "$c" && $c1 != = ]]; then
			((AP++))
			ac_mul
			if [[ $c == + ]]; then ac_bin add $l $R; else ac_bin sub $l $R; fi
			l=$R
		else R=$l; return; fi
	done
}
ac_shift() {
	local l left m
	ac_add
	l=$R
	while :; do
		ac_ws
		if [[ ${AT:AP:2} == '<<' && ${AT:AP+2:1} != = ]]; then left=1
		elif [[ ${AT:AP:2} == '>>' && ${AT:AP+2:1} != = ]]; then left=0
		else R=$l; return; fi
		((AP += 2))
		ac_add
		ac_bin and $R 63
		m=$R
		if ((left)); then ac_bin shl $l $m; else ac_bin ashr $l $m; fi
		l=$R
	done
}
ac_rel() {
	local l cc n c c1
	ac_shift
	l=$R
	while :; do
		ac_ws
		c=${AT:AP:1} c1=${AT:AP+1:1}
		if [[ $c == '<' && $c1 == = ]]; then cc=sle n=2
		elif [[ $c == '>' && $c1 == = ]]; then cc=sge n=2
		elif [[ $c == '<' && $c1 != '<' ]]; then cc=slt n=1
		elif [[ $c == '>' && $c1 != '>' ]]; then cc=sgt n=1
		else R=$l; return; fi
		((AP += n))
		ac_shift
		ac_cmp $cc $l $R
		l=$R
	done
}
ac_eq() {
	local l
	ac_rel
	l=$R
	while :; do
		ac_ws
		if [[ ${AT:AP:2} == '==' ]]; then ((AP += 2)); ac_rel; ac_cmp eq $l $R; l=$R
		elif [[ ${AT:AP:2} == '!=' ]]; then ((AP += 2)); ac_rel; ac_cmp ne $l $R; l=$R
		else R=$l; return; fi
	done
}
ac_band() {
	local l c1
	ac_eq
	l=$R
	while :; do
		ac_ws
		c1=${AT:AP+1:1}
		if [[ ${AT:AP:1} == '&' && $c1 != '&' && $c1 != = ]]; then ((AP++)); ac_eq; ac_bin and $l $R; l=$R
		else R=$l; return; fi
	done
}
ac_bxor() {
	local l
	ac_band
	l=$R
	while :; do
		ac_ws
		if [[ ${AT:AP:1} == ^ && ${AT:AP+1:1} != = ]]; then ((AP++)); ac_band; ac_bin xor $l $R; l=$R
		else R=$l; return; fi
	done
}
ac_bor() {
	local l c1
	ac_bxor
	l=$R
	while :; do
		ac_ws
		c1=${AT:AP+1:1}
		if [[ ${AT:AP:1} == '|' && $c1 != '|' && $c1 != = ]]; then ((AP++)); ac_bxor; ac_bin or $l $R; l=$R
		else R=$l; return; fi
	done
}
ac_logic() {   # is_and: && over ac_bor, else || over ac_land
	local is_and=$1 l ch id res rhs en c r c2 z
	if ((is_and)); then ch='&'; ac_bor; else ch='|'; ac_logic 1; fi
	l=$R
	while :; do
		ac_ws
		[[ ${AT:AP:1} == "$ch" && ${AT:AP+1:1} == "$ch" && ${AT:AP+2:1} != = ]] || { R=$l; return; }
		((AP += 2))
		id=$((LBL++))
		alloca_i64
		res=$R
		rhs=ac.rhs.$id en=ac.end.$id
		c=%t$((TMP++))
		emit "$c = icmp ne i64 $l, 0"
		if ((is_and)); then emit "store i64 0, ptr %t$res"; cbr ${c#%t} $rhs $en
		else emit "store i64 1, ptr %t$res"; cbr ${c#%t} $en $rhs; fi
		label $rhs
		if ((is_and)); then ac_bor; else ac_logic 1; fi
		r=$R
		c2=%t$((TMP++)) z=%t$((TMP++))
		emit "$c2 = icmp ne i64 $r, 0"
		emit "$z = zext i1 $c2 to i64"
		emit "store i64 $z, ptr %t$res"
		br $en
		label $en
		l=%t$((TMP++))
		emit "$l = load i64, ptr %t$res"
	done
}
ac_ternary() {
	local c id res t1 t2 en cv x y r
	ac_logic 0
	c=$R
	ac_ws
	if [[ ${AT:AP:1} != '?' ]]; then R=$c; return; fi
	((AP++))
	id=$((LBL++))
	alloca_i64
	res=$R
	t1=ac.t.$id t2=ac.f.$id en=ac.te.$id
	cv=%t$((TMP++))
	emit "$cv = icmp ne i64 $c, 0"
	cbr ${cv#%t} $t1 $t2
	label $t1
	ac_assign
	x=$R
	emit "store i64 $x, ptr %t$res"
	ac_ws
	if [[ ${AT:AP:1} == : ]]; then ((AP++)); else AE=1; fi
	br $en
	label $t2
	ac_ternary
	y=$R
	emit "store i64 $y, ptr %t$res"
	br $en
	label $en
	r=%t$((TMP++))
	emit "$r = load i64, ptr %t$res"
	R=$r
}
ac_assign() {
	local save mark term op= len=0 p0 p1 p2 r v o m id el sp ix key
	ac_ws
	save=$AP mark=${#FB} term=$FT
	if ac_lvalue; then
		id=$LV_ID el=$LV_EL sp=$LV_SP ix=$LV_IX key=$LV_KEY
		ac_ws
		p0=${AT:AP:1} p1=${AT:AP+1:1} p2=${AT:AP+2:1}
		if [[ $p0 == = && $p1 != = ]]; then op='=' len=1
		elif [[ -n $p0 && '+-*/%&|^' == *"$p0"* && $p1 == = ]]; then op=$p0 len=2
		elif [[ $p0$p1 == '<<' && $p2 == = ]]; then op=L len=3
		elif [[ $p0$p1 == '>>' && $p2 == = ]]; then op=R len=3
		fi
		if [[ -n $op ]]; then
			((AP += len))
			ac_assign
			r=$R v=$R
			if [[ $op != = ]]; then
				lv_load "$id" $el $sp "$ix" $key
				o=$R
				case $op in
				+) ac_bin add $o $r; v=$R ;;
				-) ac_bin sub $o $r; v=$R ;;
				'*') ac_bin mul $o $r; v=$R ;;
				/) v=%t$((TMP++)); emit "$v = call i64 @bash_rt_div(i64 $o, i64 $r)" ;;
				%) v=%t$((TMP++)); emit "$v = call i64 @bash_rt_mod(i64 $o, i64 $r)" ;;
				'&') ac_bin and $o $r; v=$R ;;
				'|') ac_bin or $o $r; v=$R ;;
				^) ac_bin xor $o $r; v=$R ;;
				L) ac_bin and $r 63; m=$R; ac_bin shl $o $m; v=$R ;;
				*) ac_bin and $r 63; m=$R; ac_bin ashr $o $m; v=$R ;;
				esac
			fi
			lv_store "$id" $el $sp "$ix" $key $v
			R=$v
			return
		fi
	fi
	FB=${FB:0:mark} FT=$term   # not an assignment: rewind
	AP=$save AE=0
	ac_ternary
}
ac_comma() {   # a, b: value of b
	local v
	ac_assign
	v=$R
	while :; do
		ac_ws
		if [[ ${AT:AP:1} != , ]]; then R=$v; return; fi
		((AP++))
		ac_assign
		v=$R
	done
}
arith_native() {   # text -> status, R = IR value
	local t=$1 sAT=$AT sAP=$AP sAE=$AE mark=${#FB} term=$FT v
	[[ $t == *'`'* || $t == *'"'* || $t == *"'"* || $t == *'\'* || $t == *'$('* ]] && return 1
	AT=$t AP=0 AE=0
	ac_comma
	v=$R
	ac_ws
	if ((AE)) || [[ -n ${AT:AP:1} || $t != $NBNL ]]; then
		FB=${FB:0:mark} FT=$term
		AT=$sAT AP=$sAP AE=$sAE
		return 1
	fi
	AT=$sAT AP=$sAP AE=$sAE
	R=$v
	return 0
}
