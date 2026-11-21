# parser.sh - bash source -> AST (words, commands, here-docs, brace expansion)
#
# Parser state: S (text), I (index), PEND (pending here-doc ids).  A sub-parser saves and restores them.
#
# Node n:  NT type, NK " children", NWS " words" (NWN count), NASG " assignments", NARR " declare name=(..)",
#          NRS " redirections", NNAME, NS1 NS2 NS3 (raw text), NCIS " case items", NFLAG, NAW0..2 (words), NEOL
# Word w:  WP " parts", WN count, WL last part
# Part p:  PK kind, PT text, PQ quoted, PO op, PA PC (operand words), PB (body node)
# Redir r: RFD fd, ROP op, RT target word          Assign a: AN name, AAPP, AVAL word, AEL " elements", AISARR
# Case item c: CIP " pattern words", CIB body       Here-doc h: HDB body word, HDD delim, HDS strip, HDQ quoted
NN=0 NWD=0 NPT=0 NRD=0 NAS=0 NCIT=0 NHD=0
S= I=0 PEND= LINEBASE=0 NOREBASE=1 SD=0

# A sub-parser works on its own text (eval string, here-doc body, $(( )) text, backticks, ...)
sub_enter() {
	SS_S[SD]=$S SS_I[SD]=$I SS_P[SD]=$PEND SS_L[SD]=$LINEBASE
	((SD++, NOREBASE++))
	S=$1 I=0 PEND= LINEBASE=0
}
sub_leave() {
	((SD--, NOREBASE--))
	S=${SS_S[SD]} I=${SS_I[SD]} PEND=${SS_P[SD]} LINEBASE=${SS_L[SD]}
	SS_S[SD]= SS_P[SD]=
}
# Drop the consumed part of the main source.  bash pays for ${S:I:1} in proportion to the length of S, so
# without this, running the compiler interpreted (the bootstrap) is quadratic in the script size.  Only done
# between commands of a list, where no caller holds a position in S; never inside a word or a sub-parser.
rebase() {
	local pre=${S:0:I} nl
	nl=${pre//$NOTNL/}
	LINEBASE=$((LINEBASE + ${#nl}))
	S=${S:I}
	I=0
}

mk() { ((++NN)); NT[NN]=$1; R=$NN; }
mkword() { ((++NWD)); WN[NWD]=0; R=$NWD; }
newpart() { ((++NPT)); PK[NPT]=$1; PT[NPT]=$2; PQ[NPT]=$3; PO[NPT]=0; R=$NPT; }
part_copy() {
	((++NPT))
	PK[NPT]=${PK[$1]} PT[NPT]=${PT[$1]} PQ[NPT]=${PQ[$1]} PO[NPT]=${PO[$1]} PA[NPT]=${PA[$1]} PC[NPT]=${PC[$1]} PB[NPT]=${PB[$1]}
	R=$NPT
}
add_part() { WP[$1]+=" $2"; ((WN[$1]++)); WL[$1]=$2; }
add_lit() {   # w text quoted force
	local l=${WL[$1]}
	if ((WN[$1] && PK[l] == 0 && PQ[l] == $3)); then PT[l]+=$2; return; fi
	[[ -z $2 ]] && (($4 == 0)) && return
	((++NPT))
	PK[NPT]=0 PT[NPT]=$2 PQ[NPT]=$3 PO[NPT]=0
	WP[$1]+=" $NPT"
	((WN[$1]++))
	WL[$1]=$NPT
}
# R = the text of a word that is a single literal part
lit_name() {
	local p
	((WN[$1] == 1)) || return 1
	p=${WL[$1]}
	((PK[p] == 0)) || return 1
	R=${PT[p]}
}

perr() {
	local pre=${S:0:I} nl line
	nl=${pre//$NOTNL/}
	line=$((LINEBASE + ${#nl} + 1))
	if ((ARMED)); then unsupported "line $line: $1"; fb_abort; fi
	die "syntax error near line $line: $1"
}
is_delim() { [[ -z $1 || $DELIMS == *"$1"* ]]; }
skip_ws() {   # nl: also newlines (flushing pending here-docs)
	local c
	while :; do
		c=${S:I:1}
		if [[ $c == ' ' || $c == $'\t' ]]; then ((I++))
		elif [[ $c == '\' && ${S:I+1:1} == $'\n' ]]; then ((I += 2))
		elif [[ $c == '#' ]]; then
			while c=${S:I:1}; [[ -n $c && $c != $'\n' ]]; do ((I++)); done
		elif [[ $c == $'\n' ]] && (($1)); then
			((I++))
			[[ -n $PEND ]] && flush_heredocs
		else return; fi
	done
}
at_kw() {
	local l=${#1} c
	[[ ${S:I:l} == "$1" ]] || return 1
	c=${S:I+l:1}
	[[ -z $c || $DELIMS == *"$c"* ]]
}
at_closer() {
	case ${S:I:1} in
	')') return 0 ;;
	';') [[ ${S:I+1:1} == ';' ]]; return ;;
	t) at_kw then; return ;;
	e) at_kw else || at_kw elif || at_kw esac; return ;;
	f) at_kw fi; return ;;
	d) at_kw do || at_kw done; return ;;
	'}') at_kw '}'; return ;;
	esac
	return 1
}

flush_heredocs() {
	local h e ln body list=$PEND c
	PEND=
	for h in $list; do
		body=
		while [[ -n ${S:I:1} ]]; do
			e=$I
			while c=${S:e:1}; [[ -n $c && $c != $'\n' ]]; do ((e++)); done
			ln=${S:I:e-I}
			if ((HDS[h])); then while [[ $ln == $'\t'* ]]; do ln=${ln:1}; done; fi
			if [[ -n $c ]]; then I=$((e + 1)); else I=$e; fi
			[[ $ln == "${HDD[h]}" ]] && break
			body+=$ln$'\n'
		done
		if ((HDQ[h])); then add_lit ${HDB[h]} "$body" 1 1
		else
			sub_enter "$body"
			scan_parts ${HDB[h]} "" 2
			sub_leave
		fi
	done
}

# $(<file): a body that is nothing but an input redirection prints the file.  It becomes `cat` with that
# redirection, flagged (NFLAG 3) so that a function named cat can never take its place.
mark_catfile() {   # body
	local s=${NK[$1]# } r
	[[ -n $s && $s != *' '* ]] || return 0
	((NT[s] == N_SIMPLE)) && [[ -z ${NWS[s]} && -z ${NASG[s]} && -z ${NARR[s]} ]] || return 0
	r=${NRS[s]# }
	[[ -n $r && $r != *' '* ]] && ((ROP[r] == R_IN && RFD[r] == 0)) || return 0
	mkword
	add_lit $R cat 0 0
	NWS[s]=" $R" NWN[s]=1 NFLAG[s]=3
}
scan_backtick() {   # w quoted
	local i=$((I + 1)) t= c n b
	while c=${S:i:1}; [[ -n $c && $c != '`' ]]; do
		if [[ $c == '\' ]]; then
			n=${S:i+1:1}
			if [[ $n == '`' || $n == '\' || $n == '$' ]]; then ((i++)); c=$n; fi
		fi
		t+=$c
		((i++))
	done
	[[ -z $c ]] && perr "unterminated backtick"
	I=$((i + 1))
	sub_enter "$t"
	parse_list
	b=$R
	sub_leave
	mark_catfile $b
	newpart $PK_CMD "" $2
	PB[R]=$b
	add_part $1 $R
}
scan_parts() {   # w stops dq   (dq: 0 plain word, 1 inside "...", 2 here-doc / $(( )) text)
	local w=$1 stops=$2 dq=$3 c n e qs sP b q=0 run sp="[\\\\'\"\$\`<~$2]"
	((dq)) && q=1
	while :; do
		c=${S:I:1}
		[[ -z $c ]] && return
		if [[ $c != $sp ]]; then   # a run of ordinary characters (sp: everything special here, and the stops)
			run=${S:I:128}
			run=${run%%$sp*}
			add_lit $w "$run" $q 0
			I=$((I + ${#run}))
			continue
		fi
		if ((dq == 0)); then
			if [[ $stops == *"$c"* ]] && [[ $c != '<' || ${S:I+1:1} != '(' ]]; then return; fi
		elif ((dq == 1)) && [[ $c == '"' ]]; then return
		fi
		case $c in
		'\')
			n=${S:I+1:1}
			if ((dq == 0)); then
				if [[ $n == $'\n' ]]; then ((I += 2))
				elif [[ -z $n ]]; then add_lit $w '\' 1 0; ((I++))
				else add_lit $w "$n" 1 0; ((I += 2)); fi
			else
				e=0
				case $n in
				'$' | '`' | '\' | $'\n') e=1 ;;
				'"') ((dq == 1)) && e=1 ;;
				esac
				if ((e)); then
					[[ $n != $'\n' ]] && add_lit $w "$n" 1 0
					((I += 2))
				else add_lit $w '\' 1 0; ((I++)); fi
			fi
			;;
		"'")
			if ((dq == 0)); then
				e=$((I + 1))
				while c=${S:e:1}; [[ -n $c && $c != "'" ]]; do ((e++)); done
				[[ -z $c ]] && perr "unterminated single quote"
				add_lit $w "${S:I+1:e-I-1}" 1 1
				I=$((e + 1))
			else add_lit $w "$c" $q 0; ((I++)); fi
			;;
		'"')
			if ((dq == 0)); then
				((I++))
				qs=$I
				scan_parts $w "" 1
				((I == qs)) && add_lit $w "" 1 1   # "" is an empty word; "$@" is not
				[[ ${S:I:1} != '"' ]] && perr "unterminated double quote"
				((I++))
			else add_lit $w "$c" $q 0; ((I++)); fi
			;;
		'$') scan_dollar $w $q ;;
		'`') scan_backtick $w $q ;;
		'<')
			if ((dq == 0)) && [[ ${S:I+1:1} == '(' ]]; then
				sP=$PEND
				PEND=
				((I += 2))
				parse_list
				b=$R
				skip_ws 1
				[[ ${S:I:1} != ')' ]] && perr "expected ) after <("
				((I++))
				PEND=$sP
				newpart $PK_PROC "" 0
				PB[R]=$b
				add_part $w $R
			else add_lit $w "$c" $q 0; ((I++)); fi
			;;
		'~')
			n=${S:I+1:1}
			if ((dq == 0 && WN[w] == 0)) && [[ $n == [a-zA-Z0-9+_-] ]]; then
				unsupported "~user / ~+ / ~- tilde expansion"
				add_lit $w "$c" 0 0
				((I++))
			elif ((dq == 0 && WN[w] == 0)) && [[ -z $n || $n == / || $stops == *"$n"* ]]; then
				newpart $PK_VAR HOME 1
				add_part $w $R
				((I++))
			else add_lit $w "$c" $q 0; ((I++)); fi
			;;
		*) add_lit $w "$c" $q 0; ((I++)) ;;
		esac
	done
}
ansi_c() {   # w ; at $'
	local i=$((I + 2)) t= c v k
	while c=${S:i:1}; [[ -n $c && $c != "'" ]]; do
		((i++))
		if [[ $c != '\' || -z ${S:i:1} ]]; then t+=$c; continue; fi
		c=${S:i:1}
		((i++))
		case $c in
		n) t+=$'\n' ;; t) t+=$'\t' ;; r) t+=$'\r' ;; a) t+=$'\a' ;; b) t+=$'\b' ;;
		e | E) t+=$'\e' ;; f) t+=$'\f' ;; v) t+=$'\v' ;;
		x)
			v=0 k=0
			while ((k < 2)) && [[ ${S:i:1} == [0-9a-fA-F] ]]; do v=$((v * 16 + 16#${S:i:1})); ((i++, k++)); done
			chr $v
			t+=$R
			;;
		[0-7])
			v=$((c)) k=1
			while ((k < 3)) && [[ ${S:i:1} == [0-7] ]]; do v=$((v * 8 + ${S:i:1})); ((i++, k++)); done
			chr $v
			t+=$R
			;;
		*) t+=$c ;;
		esac
	done
	if [[ -n $c ]]; then I=$((i + 1)); else I=$i; fi
	add_lit $1 "$t" 1 1
}
scan_param() {   # w quoted ; at ${
	local w=$1 quoted=$2 i=$((I + 2)) len=0 ind=0 st c c2 d text p op=0 subst=0 hasop=1
	c=${S:i:1} c2=${S:i+1:1}
	if [[ $c == '#' && -n $c2 && $c2 != '}' ]]; then len=1; ((i++))
	elif [[ $c == '!' && -n $c2 && $c2 != '}' ]]; then ind=1; ((i++)); fi
	st=$i
	c=${S:i:1}
	if [[ $c == [a-zA-Z_] ]]; then
		while [[ ${S:i:1} == [a-zA-Z0-9_] ]]; do ((i++)); done
	elif [[ $c == [0-9] ]]; then
		while [[ ${S:i:1} == [0-9] ]]; do ((i++)); done
	elif [[ -n $c && '@*#?$!-' == *"$c"* ]]; then ((i++))
	else perr "bad substitution"; fi
	if [[ ${S:i:1} == '[' ]]; then
		d=0
		while :; do
			c=${S:i:1}
			if [[ $c == '[' ]]; then ((d++)); elif [[ $c == ']' ]]; then ((d--)); fi
			((i++))
			[[ -n ${S:i:1} ]] && ((d > 0)) || break
		done
	fi
	text=${S:st:i-st}
	newpart $PK_PARAM "$text" $quoted
	p=$R
	c=${S:i:1} c2=${S:i+1:1}
	case $c in
	'}') hasop=0 ;;
	':')
		case $c2 in
		'-') op=$P_DEFAULT; ((i += 2)) ;;
		'=') op=$P_ASSIGN; ((i += 2)) ;;
		'+') op=$P_ALT; ((i += 2)) ;;
		'?') op=$P_ERR; ((i += 2)) ;;
		*) op=$P_SUBSTR; ((i++)) ;;
		esac
		;;
	'-') op=$P_DEFAULT_NC; ((i++)) ;;
	'=') op=$P_ASSIGN_NC; ((i++)) ;;
	'+') op=$P_ALT_NC; ((i++)) ;;
	'?') op=$P_ERR_NC; ((i++)) ;;
	'#') if [[ $c2 == '#' ]]; then op=$P_TRIM_L; ((i += 2)); else op=$P_TRIM_S; ((i++)); fi ;;
	'%') if [[ $c2 == '%' ]]; then op=$P_TRIM_LE; ((i += 2)); else op=$P_TRIM_SE; ((i++)); fi ;;
	'/')
		subst=1
		case $c2 in
		'/') op=$P_SUBST_ALL; ((i += 2)) ;;
		'#') op=$P_SUBST_PRE; ((i += 2)) ;;
		'%') op=$P_SUBST_SUF; ((i += 2)) ;;
		*) op=$P_SUBST; ((i++)) ;;
		esac
		;;
	'^') if [[ $c2 == '^' ]]; then op=$P_UPALL; ((i += 2)); else op=$P_UP1; ((i++)); fi ;;
	',') if [[ $c2 == ',' ]]; then op=$P_LOWALL; ((i += 2)); else op=$P_LOW1; ((i++)); fi ;;
	*) perr "bad parameter expansion" ;;
	esac
	if ((hasop)); then
		I=$i
		mkword
		PA[p]=$R
		if ((subst)); then scan_parts ${PA[p]} "/}" 0; else scan_parts ${PA[p]} "}" 0; fi
		if ((subst)) && [[ ${S:I:1} == / ]]; then
			((I++))
			mkword
			PC[p]=$R
			scan_parts ${PC[p]} "}" 0
		fi
		i=$I
	fi
	[[ ${S:i:1} != '}' ]] && perr "missing } in parameter expansion"
	I=$((i + 1))
	if ((len)); then op=$P_LEN
	elif ((ind && op == P_PLAIN)); then
		if [[ $text == *'[@]'* || $text == *'[*]'* ]]; then op=$P_KEYS; else op=$P_INDIRECT; fi
	fi
	PO[p]=$op
	add_part $w $p
}
scan_dollar() {   # w quoted ; at $
	local w=$1 quoted=$2 n=${S:I+1:1} j d c text p sP b e
	if [[ $n == '(' && ${S:I+2:1} == '(' ]]; then
		j=$((I + 3)) d=0
		while c=${S:j:1}; [[ -n $c ]]; do
			if [[ $c == '(' ]]; then ((d++))
			elif [[ $c == ')' ]]; then
				((d == 0)) && [[ ${S:j+1:1} == ')' ]] && break
				((d--))
			fi
			((j++))
		done
		[[ -z $c ]] && perr 'unterminated $(('
		text=${S:I+3:j-I-3}
		newpart $PK_ARITH "$text" $quoted
		p=$R
		if [[ $text == *'$'* || $text == *'`'* ]]; then   # expand ${#x}, $(cmd), ... first
			sub_enter "$text"
			mkword
			PA[p]=$R
			scan_parts $R "" 2
			sub_leave
		fi
		add_part $w $p
		I=$((j + 2))
	elif [[ $n == '(' ]]; then
		sP=$PEND
		PEND=
		((I += 2))
		parse_list
		b=$R
		skip_ws 1
		[[ ${S:I:1} != ')' ]] && perr 'expected ) after $('
		((I++))
		PEND=$sP
		mark_catfile $b
		newpart $PK_CMD "" $quoted
		PB[R]=$b
		add_part $w $R
	elif [[ $n == '[' ]]; then
		unsupported '$[...] arithmetic'
		add_lit $w '$' $quoted 0
		((I++))
	elif [[ $n == '{' ]]; then scan_param $w $quoted
	elif ((!quoted)) && [[ $n == "'" ]]; then ansi_c $w
	elif [[ $n == [a-zA-Z_] ]]; then
		e=$((I + 1))
		while [[ ${S:e:1} == [a-zA-Z0-9_] ]]; do ((e++)); done
		newpart $PK_VAR "${S:I+1:e-I-1}" $quoted
		add_part $w $R
		I=$e
	elif [[ $n == [0-9] ]] || [[ -n $n && '@*#?$!-' == *"$n"* ]]; then
		newpart $PK_VAR "$n" $quoted
		add_part $w $R
		((I += 2))
	else
		add_lit $w '$' $quoted 0
		((I++))
	fi
}
parse_word() {   # stops -> R = word, 0 if nothing was consumed
	local w st=$I
	mkword
	w=$R
	((NOREBASE++))
	scan_parts $w "$1" 0
	((NOREBASE--))
	if ((I == st)); then R=0; else R=$w; fi
}

# ---- brace expansion (literal, unquoted parts only): appends the resulting words to BXO ----
make_seq() {   # text -> SEQ, status
	local b=$1 t2 d3 step=1 a z pad=0 v x ca cz
	[[ $b == *..* ]] || return 1
	t2=${b#*..}
	b=${b%%..*}
	if [[ $t2 == *..* ]]; then
		d3=${t2#*..}
		t2=${t2%%..*}
		strtol_full "$d3" || return 1
		step=$R
		((step == 0)) && return 1
		((step < 0)) && step=$((-step))
	fi
	if [[ -n $b && -n $t2 ]] && strtol_full "$b"; then
		a=$R
		if strtol_full "$t2"; then
			z=$R
			if { ((${#b} > 1)) && [[ $b == 0* || $b == -0* ]]; } || { ((${#t2} > 1)) && [[ $t2 == 0* || $t2 == -0* ]]; }; then
				pad=${#b}
				((${#t2} > pad)) && pad=${#t2}
			fi
			SEQ=()
			if ((a <= z)); then
				for ((v = a; v <= z; v += step)); do
					if ((pad)); then printf -v x "%0${pad}d" $v; else x=$v; fi
					SEQ+=("$x")
				done
			else
				for ((v = a; v >= z; v -= step)); do
					if ((pad)); then printf -v x "%0${pad}d" $v; else x=$v; fi
					SEQ+=("$x")
				done
			fi
			return 0
		fi
	fi
	if ((${#b} == 1 && ${#t2} == 1)) && [[ $b == [a-zA-Z] && $t2 == [a-zA-Z] ]]; then
		ord "$b"
		ca=$R
		ord "$t2"
		cz=$R
		SEQ=()
		if ((ca <= cz)); then
			for ((v = ca; v <= cz; v += step)); do chr $v; SEQ+=("$R"); done
		else
			for ((v = ca; v >= cz; v -= step)); do chr $v; SEQ+=("$R"); done
		fi
		return 0
	fi
	return 1
}
brace_expand() {   # w
	local w=$1 k p t L a b d st c ch alt nw j np
	local -a parts alts
	parts=(${WP[w]})
	np=${#parts[@]}
	for ((k = 0; k < np; k++)); do
		p=${parts[k]}
		((PK[p] != 0 || PQ[p])) && continue
		t=${PT[p]}
		L=${#t}
		for ((a = 0; a < L; a++)); do
			[[ ${t:a:1} != '{' ]] && continue
			d=0
			for ((b = a; b < L; b++)); do
				ch=${t:b:1}
				if [[ $ch == '{' ]]; then ((d++)); elif [[ $ch == '}' ]]; then ((--d == 0)) && break; fi
			done
			((b >= L)) && continue
			alts=()
			st=$((a + 1)) d=0
			for ((c = a + 1; c <= b; c++)); do
				ch=${t:c:1}
				if ((c == b)) || { [[ $ch == , ]] && ((d == 0)); }; then
					alts+=("${t:st:c-st}")
					st=$((c + 1))
				elif [[ $ch == '{' ]]; then ((d++))
				elif [[ $ch == '}' ]]; then ((d--)); fi
			done
			if ((${#alts[@]} <= 1)); then
				make_seq "${t:a+1:b-a-1}" || continue
				alts=("${SEQ[@]}")
			fi
			for alt in "${alts[@]}"; do
				mkword
				nw=$R
				for ((j = 0; j < np; j++)); do
					if ((j != k)); then add_part $nw ${parts[j]}; continue; fi
					newpart $PK_LIT "${t:0:a}$alt${t:b+1}" 0
					add_part $nw $R
				done
				brace_expand $nw
			done
			return
		done
	done
	BXO+=($w)
}
push_words() {   # node word
	local x
	BXO=()
	brace_expand $2
	for x in "${BXO[@]}"; do
		NWS[$1]+=" $x"
		((NWN[$1]++))
	done
}

# ---- redirections ----
try_redir() {   # node -> status 0 if a redirection was parsed
	local n=$1 j=$I fd=-1 op=-1 adv=0 strip=0 dflt=1 c c1 c2 r hd d q
	while [[ ${S:j:1} == [0-9] ]]; do ((j++)); done
	c=${S:j:1}
	if ((j > I)) && [[ $c == '<' || $c == '>' ]]; then c_atoi "${S:I:j-I}"; fd=$R
	else j=$I; c=${S:j:1}; fi
	c1=${S:j+1:1} c2=${S:j+2:1}
	if ((fd < 0)) && [[ $c == '&' && $c1 == '>' ]]; then
		if [[ $c2 == '>' ]]; then op=$R_APPEND_ERR adv=3; else op=$R_OUT_ERR adv=2; fi
	elif [[ $c == '<' ]]; then
		dflt=0
		if [[ $c1 == '<' && $c2 == '<' ]]; then op=$R_HERESTR adv=3
		elif [[ $c1 == '<' && $c2 == '-' ]]; then op=$R_HEREDOC adv=3 strip=1
		elif [[ $c1 == '<' ]]; then op=$R_HEREDOC adv=2
		elif [[ $c1 == '&' ]]; then op=$R_DUP_IN adv=2
		elif [[ $c1 == '>' ]]; then op=$R_RDWR adv=2
		elif [[ $c1 == '(' ]] && ((fd < 0)); then return 1
		else op=$R_IN adv=1; fi
	elif [[ $c == '>' ]]; then
		if [[ $c1 == '>' ]]; then op=$R_APPEND adv=2
		elif [[ $c1 == '&' ]]; then op=$R_DUP_OUT adv=2
		elif [[ $c1 == '|' ]]; then op=$R_CLOBBER adv=2
		else op=$R_OUT adv=1; fi
	fi
	((op < 0)) && return 1
	I=$((j + adv))
	skip_ws 0
	if ((fd < 0)); then if ((dflt)); then fd=1; else fd=0; fi; fi
	((++NRD))
	r=$NRD
	RFD[r]=$fd ROP[r]=$op
	if ((op == R_HEREDOC)); then
		((++NHD))
		hd=$NHD
		HDS[hd]=$strip HDQ[hd]=0
		mkword
		HDB[hd]=$R
		d=
		while c=${S:I:1}; [[ -n $c && $DELIMS != *"$c"* ]]; do
			if [[ $c == "'" || $c == '"' ]]; then
				HDQ[hd]=1 q=$c
				((I++))
				while c=${S:I:1}; [[ -n $c && $c != "$q" ]]; do d+=$c; ((I++)); done
				[[ -n $c ]] && ((I++))
			elif [[ $c == '\' ]]; then
				HDQ[hd]=1
				((I++))
				c=${S:I:1}
				if [[ -n $c ]]; then d+=$c; ((I++)); fi
			else d+=$c; ((I++)); fi
		done
		HDD[hd]=$d
		RT[r]=${HDB[hd]}
		PEND+=" $hd"
	else
		parse_word "$WSTOP"
		((R)) || perr "missing redirection target"
		RT[r]=$R
	fi
	NRS[n]+=" $r"
	return 0
}
parse_redirs() { while :; do skip_ws 0; try_redir $1 || break; done; }

# ---- commands ----
parse_assign_word() {   # word declare_arg -> status, R = assignment
	local w=$1 da=$2 p0 t j=0 d c app=0 rest a v e k
	local -a ps
	((WN[w])) || return 1
	ps=(${WP[w]})
	p0=${ps[0]}
	((PK[p0] == 0 && !PQ[p0])) || return 1
	t=${PT[p0]}
	[[ ${t:0:1} == [a-zA-Z_] ]] || return 1
	while [[ ${t:j:1} == [a-zA-Z0-9_] ]]; do ((j++)); done
	if [[ ${t:j:1} == '[' ]]; then
		d=0
		while :; do
			c=${t:j:1}
			if [[ $c == '[' ]]; then ((d++)); elif [[ $c == ']' ]]; then ((d--)); fi
			((j++))
			[[ -n ${t:j:1} ]] && ((d > 0)) || break
		done
		((d)) && return 1
	fi
	if [[ ${t:j:2} == += ]]; then app=1; ((j++)); fi
	[[ ${t:j:1} == = ]] || return 1
	((++NAS))
	a=$NAS
	AN[a]=${t:0:j-app} AAPP[a]=$app
	rest=${t:j+1}
	if [[ -z $rest ]] && ((WN[w] == 1)) && [[ ${S:I:1} == '(' ]]; then   # name=( ... )
		((I++))
		AISARR[a]=1
		while :; do
			skip_ws 1
			if [[ ${S:I:1} == ')' ]]; then ((I++)); break; fi
			parse_word "$WSTOP"
			e=$R
			((e)) || perr "bad array literal"
			BXO=()
			brace_expand $e
			AEL[a]+=" ${BXO[*]}"
		done
		R=$a
		return 0
	fi
	((da)) && return 1
	mkword
	v=$R
	if [[ ${rest:0:1} == '~' ]] && [[ -z ${rest:1:1} || ${rest:1:1} == / || ${rest:1:1} == : ]]; then
		newpart $PK_VAR HOME 1
		add_part $v $R
		rest=${rest:1}
	elif [[ ${rest:0:1} == '~' && -n ${rest:1:1} ]]; then
		unsupported "~user tilde expansion in assignment"
	fi
	[[ -n $rest ]] && add_lit $v "$rest" 0 0
	for ((k = 1; k < ${#ps[@]}; k++)); do add_part $v ${ps[k]}; done
	AVAL[a]=$v
	R=$a
	return 0
}
# name[subscript]=value where the subscript holds expansions or quotes ($i, "a b", $(cmd)): recognized on the
# raw source, since the word parser would split it into parts.  AN keeps the raw "name[subscript]".
parse_subscript_assign() {
	local i=$I j d=1 q= c e app=0 sub a w v p0 rest k
	local -a ps
	[[ ${S:i:1} == [a-zA-Z_] ]] || return 1
	while [[ ${S:i:1} == [a-zA-Z0-9_] ]]; do ((i++)); done
	[[ ${S:i:1} == '[' ]] || return 1
	j=$((i + 1))
	while c=${S:j:1}; [[ -n $c ]]; do
		if [[ -n $q ]]; then
			if [[ $c == "$q" ]]; then q=
			elif [[ $c == '\' && $q == '"' && -n ${S:j+1:1} ]]; then ((j++)); fi
			((j++))
			continue
		fi
		if [[ $c == '\' && -n ${S:j+1:1} ]]; then ((j += 2)); continue; fi
		if [[ $c == "'" || $c == '"' ]]; then q=$c
		elif [[ $c == '[' ]]; then ((d++))
		elif [[ $c == ']' ]]; then ((--d == 0)) && break
		elif [[ $c == $'\n' ]]; then return 1; fi
		((j++))
	done
	[[ ${S:j:1} == ']' ]] || return 1
	e=$((j + 1))
	if [[ ${S:e:2} == += ]]; then app=1; ((e++)); fi
	[[ ${S:e:1} == = ]] || return 1
	sub=${S:i:j-i}
	[[ $sub == *'$'* || $sub == *'`'* || $sub == *"'"* || $sub == *'"'* || $sub == *'\'* || $sub == *' '* || $sub == *$'\t'* ]] || return 1
	((++NAS))
	a=$NAS
	AN[a]=${S:I:j+1-I} AAPP[a]=$app
	I=$((e + 1))
	if [[ ${S:I:1} == '(' ]]; then unsupported "array literal assigned to an element"; return 1; fi
	if is_delim "${S:I:1}"; then
		mkword
		AVAL[a]=$R
		add_lit $R "" 1 1
		R=$a
		return 0
	fi
	parse_word "$WSTOP"
	w=$R
	mkword
	v=$R
	if ((w && WN[w])); then
		ps=(${WP[w]})
		p0=${ps[0]}
		rest=${PT[p0]}
		if ((PK[p0] == 0 && !PQ[p0])) && [[ ${rest:0:1} == '~' ]] && [[ -z ${rest:1:1} || ${rest:1:1} == / || ${rest:1:1} == : ]]; then
			newpart $PK_VAR HOME 1
			add_part $v $R
			[[ -n ${rest:1} ]] && add_lit $v "${rest:1}" 0 0
			for ((k = 1; k < ${#ps[@]}; k++)); do add_part $v ${ps[k]}; done
			AVAL[a]=$v
			R=$a
			return 0
		fi
	fi
	if ((w)); then AVAL[a]=$w; else AVAL[a]=$v; fi
	((WN[AVAL[a]])) || add_lit ${AVAL[a]} "" 1 1
	R=$a
	return 0
}
parse_simple() {
	local n seen=0 c save w fw
	mk $N_SIMPLE
	n=$R
	while :; do
		skip_ws 0
		try_redir $n && continue
		c=${S:I:1}
		[[ -z $c || $'\n;|)&(' == *"$c"* ]] && break
		if ((!seen)); then
			save=$I
			if parse_subscript_assign; then NASG[n]+=" $R"; continue; fi
			I=$save
		fi
		parse_word "$WSTOP"
		w=$R
		((w)) || perr "unexpected token"
		if ((!seen)) && parse_assign_word $w 0; then NASG[n]+=" $R"; continue; fi
		if ((seen)); then
			fw=${NWS[n]# }
			fw=${fw%% *}
			if lit_name $fw; then
				case $R in
				declare | local | typeset | readonly | export)
					if parse_assign_word $w 1; then NARR[n]+=" $R"; continue; fi
					;;
				esac
			fi
		fi
		seen=1
		push_words $n $w
	done
	R=$n
}
expect_kw() { skip_ws 1; at_kw "$1" || perr "expected '$1'"; I=$((I + ${#1})); }
parse_cond_then() {   # if / elif
	local n
	mk $N_IF
	n=$R
	parse_list
	NK[n]+=" $R"
	expect_kw then
	parse_list
	NK[n]+=" $R"
	skip_ws 1
	if at_kw elif; then
		((I += 4))
		parse_cond_then
		NK[n]+=" $R"
	else
		if at_kw else; then
			((I += 4))
			parse_list
			NK[n]+=" $R"
		fi
		expect_kw fi
	fi
	R=$n
}
read_raw_arith() {   # stop -> R = raw text up to the stop char at depth 0 (")" means "))"), CLOSED
	local stop=$1 st=$I d=0 c
	CLOSED=0
	while c=${S:I:1}; [[ -n $c ]]; do
		if [[ $c == '(' ]]; then ((d++))
		elif [[ $c == ')' ]]; then
			if ((d == 0)); then
				if [[ $stop == ')' && ${S:I+1:1} == ')' ]]; then CLOSED=1; break; fi
			else ((d--)); fi
		elif ((d == 0)) && [[ $c == "$stop" && $stop != ')' ]]; then CLOSED=1; break
		fi
		((I++))
	done
	R=${S:st:I-st}
}
arith_word() {   # (( )) text that needs $-expansion before evaluation -> R = word or 0
	local w
	if [[ $1 != *'$'* && $1 != *'`'* ]]; then R=0; return; fi
	sub_enter "$1"
	mkword
	w=$R
	scan_parts $w "" 2
	sub_leave
	R=$w
}
parse_for() {
	local n nm c w name
	skip_ws 0
	if [[ ${S:I:2} == '((' ]]; then
		((I += 2))
		mk $N_CFOR
		n=$R
		read_raw_arith ';'
		NS1[n]=$R
		((I++))
		read_raw_arith ';'
		NS2[n]=$R
		((I++))
		read_raw_arith ')'
		NS3[n]=$R
		((CLOSED)) || perr "bad for (( ))"
		((I += 2))
		skip_ws 0
		[[ ${S:I:1} == ';' ]] && ((I++))
		arith_word "${NS1[n]}"
		NAW0[n]=$R
		arith_word "${NS2[n]}"
		NAW1[n]=$R
		arith_word "${NS3[n]}"
		NAW2[n]=$R
		expect_kw do
		parse_list
		NK[n]=" $R"
		expect_kw done
		R=$n
		return
	fi
	parse_word "$WSTOP"
	nm=$R
	((nm)) && lit_name $nm || perr "bad for variable"
	name=$R
	mk $N_FOR
	n=$R
	NNAME[n]=$name
	skip_ws 0
	if at_kw in; then
		((I += 2))
		NFLAG[n]=1
		while :; do
			skip_ws 0
			c=${S:I:1}
			[[ -z $c || $c == ';' || $c == $'\n' ]] && break
			parse_word "$WSTOP"
			w=$R
			((w)) || perr "bad for list"
			push_words $n $w
		done
	fi
	skip_ws 0
	[[ ${S:I:1} == ';' ]] && ((I++))
	expect_kw do
	parse_list
	NK[n]=" $R"
	expect_kw done
	R=$n
}
parse_case() {
	local n w it pw
	mk $N_CASE
	n=$R
	skip_ws 0
	parse_word "$WSTOP"
	w=$R
	((w)) || perr "bad case word"
	NWS[n]=" $w" NWN[n]=1
	expect_kw in
	while :; do
		skip_ws 1
		while [[ ${S:I:1} == ';' ]]; do ((I++)); skip_ws 1; done
		if at_kw esac; then ((I += 4)); break; fi
		[[ -z ${S:I:1} ]] && perr "unterminated case"
		((++NCIT))
		it=$NCIT
		if [[ ${S:I:1} == '(' ]]; then ((I++)); skip_ws 0; fi
		while :; do
			skip_ws 0
			parse_word "$WSTOP"
			pw=$R
			((pw)) || perr "bad case pattern"
			CIP[it]+=" $pw"
			skip_ws 0
			if [[ ${S:I:1} == '|' ]]; then ((I++)); continue; fi
			break
		done
		[[ ${S:I:1} == ')' ]] || perr "expected ) in case"
		((I++))
		parse_list
		CIB[it]=$R
		NCIS[n]+=" $it"
		skip_ws 1
		if [[ ${S:I:2} == ';;' ]]; then
			((I += 2))
			if [[ ${S:I:1} == '&' ]]; then unsupported "case ;;& fallthrough"; ((I++)); fi
		fi
	done
	R=$n
}
parse_dbl() {
	local n after_re=0 w c c2
	mk $N_DBL
	n=$R
	while :; do
		skip_ws 1
		[[ -z ${S:I:1} ]] && perr "unterminated [["
		if at_kw ']]'; then ((I += 2)); break; fi
		c=${S:I:1} c2=${S:I+1:1}
		if ((after_re)); then
			parse_word $' \t\n'
			w=$R
			after_re=0
		elif [[ $c$c2 == '&&' || $c$c2 == '||' ]]; then
			mkword
			w=$R
			add_lit $w "$c$c2" 0 1
			((I += 2))
		elif [[ $c == '(' || $c == ')' || $c == '<' || $c == '>' ]]; then
			mkword
			w=$R
			add_lit $w "$c" 0 1
			((I++))
		else
			parse_word "$WSTOP"
			w=$R
		fi
		((w)) || perr "bad [[ expression"
		lit_name $w && [[ $R == '=~' ]] && after_re=1
		NWS[n]+=" $w"
		((NWN[n]++))
	done
	R=$n
}
parse_command() {
	local n e c j k name un nm
	skip_ws 0
	if [[ ${S:I:2} == '((' ]]; then
		((I += 2))
		read_raw_arith ')'
		e=$R
		if ((CLOSED)); then
			((I += 2))
			mk $N_ARITH
			n=$R
			NS1[n]=$e
			arith_word "$e"
			NAW0[n]=$R
			parse_redirs $n
			R=$n
			return
		fi
		I=$((I - 2 - ${#e}))   # really a nested subshell
	fi
	c=${S:I:1}
	case $c in
	'(')
		((I++))
		mk $N_SUB
		n=$R
		parse_list
		NK[n]=" $R"
		skip_ws 1
		[[ ${S:I:1} != ')' ]] && perr "expected )"
		((I++))
		parse_redirs $n
		R=$n
		return
		;;
	'{')
		if at_kw '{'; then
			((I++))
			mk $N_GROUP
			n=$R
			parse_list
			NK[n]=" $R"
			expect_kw '}'
			parse_redirs $n
			R=$n
			return
		fi
		;;
	i)
		if at_kw if; then
			((I += 2))
			parse_cond_then
			n=$R
			parse_redirs $n
			R=$n
			return
		fi
		;;
	w | u)
		if at_kw while || at_kw until; then
			un=0
			[[ $c == u ]] && un=1
			((I += 5))
			if ((un)); then mk $N_UNTIL; else mk $N_WHILE; fi
			n=$R
			parse_list
			NK[n]+=" $R"
			expect_kw do
			parse_list
			NK[n]+=" $R"
			expect_kw done
			parse_redirs $n
			R=$n
			return
		fi
		;;
	f)
		if at_kw for; then
			((I += 3))
			parse_for
			n=$R
			parse_redirs $n
			R=$n
			return
		fi
		;;
	c)
		if at_kw case; then
			((I += 4))
			parse_case
			n=$R
			parse_redirs $n
			R=$n
			return
		fi
		;;
	esac
	if at_kw select || at_kw coproc || at_kw time; then unsupported "'${S:I:6}' keyword"; fi
	if at_kw '[['; then
		((I += 2))
		parse_dbl
		n=$R
		parse_redirs $n
		R=$n
		return
	fi
	if at_kw function; then
		((I += 8))
		skip_ws 0
		parse_word "$WSTOP"
		nm=$R
		((nm)) && lit_name $nm || perr "bad function name"
		name=$R
		mk $N_FUNC
		n=$R
		NNAME[n]=$name
		skip_ws 0
		if [[ ${S:I:1} == '(' ]]; then
			((I++))
			skip_ws 0
			[[ ${S:I:1} != ')' ]] && perr "expected )"
			((I++))
		fi
		skip_ws 1
		parse_command
		NK[n]=" $R"
		R=$n
		return
	fi
	if [[ $c == [a-zA-Z_] ]]; then   # name() compound
		j=$I
		while [[ ${S:j:1} == [a-zA-Z0-9_.:-] ]]; do ((j++)); done
		e=$j
		while [[ ${S:j:1} == ' ' || ${S:j:1} == $'\t' ]]; do ((j++)); done
		if [[ ${S:j:1} == '(' ]]; then
			k=$((j + 1))
			while [[ ${S:k:1} == ' ' || ${S:k:1} == $'\t' ]]; do ((k++)); done
			if [[ ${S:k:1} == ')' ]]; then
				mk $N_FUNC
				n=$R
				NNAME[n]=${S:I:e-I}
				I=$((k + 1))
				skip_ws 1
				parse_command
				NK[n]=" $R"
				R=$n
				return
			fi
		fi
	fi
	parse_simple
}
parse_pipeline() {
	local neg=0 first res
	skip_ws 0
	while at_kw '!'; do
		neg=$((!neg))
		((I++))
		skip_ws 0
	done
	parse_command
	first=$R res=$R
	skip_ws 0
	if [[ ${S:I:1} == '|' && ${S:I+1:1} != '|' ]]; then
		mk $N_PIPE
		res=$R
		NK[res]=" $first"
		while [[ ${S:I:1} == '|' && ${S:I+1:1} != '|' ]]; do
			((I++))
			[[ ${S:I:1} == '&' ]] && ((I++))
			skip_ws 1
			parse_command
			NK[res]+=" $R"
			skip_ws 0
		done
	fi
	if ((neg)); then
		mk $N_NOT
		NK[R]=" $res"
		return
	fi
	R=$res
}
parse_andor() {
	local l t n
	parse_pipeline
	l=$R
	while :; do
		skip_ws 0
		if [[ ${S:I:2} == '&&' ]]; then t=$N_AND
		elif [[ ${S:I:2} == '||' ]]; then t=$N_OR
		else R=$l; return; fi
		((I += 2))
		skip_ws 1
		mk $t
		n=$R
		parse_pipeline
		NK[n]=" $l $R"
		l=$n
	done
}
parse_list() {
	local l c j last
	mk $N_LIST
	l=$R
	while :; do
		((I > 4096 && !NOREBASE)) && rebase
		skip_ws 1
		if [[ ${S:I:2} == ';&' ]]; then unsupported "case ;& fallthrough"; perr "case ;&"; fi
		while [[ ${S:I:1} == ';' && ${S:I+1:1} != ';' ]]; do ((I++)); skip_ws 1; done
		[[ -z ${S:I:1} ]] && break
		at_closer && break
		parse_andor
		last=$R
		skip_ws 0
		c=${S:I:1}
		if [[ $c == '&' && ${S:I+1:1} != '&' ]]; then
			((I++))
			mk $N_BG
			NK[R]=" $last"
			last=$R
		elif [[ $c == ';' && ${S:I+1:1} != ';' ]]; then ((I++))
		elif [[ $c == $'\n' || -z $c ]]; then :
		elif ! at_closer; then perr "unexpected token"
		fi
		j=$I
		while [[ ${S:j:1} == ' ' || ${S:j:1} == $'\t' ]]; do ((j++)); done
		if [[ ${S:j:1} == '#' ]]; then
			while c=${S:j:1}; [[ -n $c && $c != $'\n' ]]; do ((j++)); done
		fi
		[[ ${S:j:1} == $'\n' || -z ${S:j:1} ]] && NEOL[last]=1
		NK[l]+=" $last"
	done
	R=$l
}
parse_text() {   # eval / trap strings
	local l
	sub_enter "$1"
	parse_list
	l=$R
	skip_ws 1
	[[ -n ${S:I:1} ]] && perr "unexpected token in eval/trap string"
	sub_leave
	R=$l
}
