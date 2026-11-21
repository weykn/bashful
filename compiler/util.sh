# util.sh - constants, diagnostics, the embedded tool table, IR string escaping, hashing
#
# The compiler is written in the subset of bash that bashful compiles natively, and is compiled by itself:
#   * functions return results in globals (R, and a few named ones) instead of $(...)
#   * AST nodes, words and parts are integer ids into parallel global arrays
#   * child / part lists are space separated id strings (" 3 7 12")
#   * every temporary is `local`: callers may rely on their locals surviving any call
LC_ALL=C   # bytes, not characters (not exported: programs run with -r keep the user's locale)

# ---- ABI with the runtime (runtime/bash_runtime.h) ----
WB_ARGV=0 WB_STR=1 WB_PATTERN=2 WB_REGEX=3
R_IN=0 R_OUT=1 R_APPEND=2 R_DUP_IN=3 R_DUP_OUT=4 R_RDWR=5 R_HEREDOC=6 R_CLOBBER=7 R_HERESTR=8 R_OUT_ERR=9 R_APPEND_ERR=10
P_PLAIN=0 P_DEFAULT=1 P_DEFAULT_NC=2 P_ASSIGN=3 P_ASSIGN_NC=4 P_ALT=5 P_ALT_NC=6 P_ERR=7 P_ERR_NC=8 P_LEN=9
P_TRIM_S=10 P_TRIM_L=11 P_TRIM_SE=12 P_TRIM_LE=13 P_SUBST=14 P_SUBST_ALL=15 P_SUBST_PRE=16 P_SUBST_SUF=17 P_SUBSTR=18
P_UP1=19 P_UPALL=20 P_LOW1=21 P_LOWALL=22 P_KEYS=23 P_INDIRECT=24

# ---- AST ----
PK_LIT=0 PK_VAR=1 PK_PARAM=2 PK_CMD=3 PK_ARITH=4 PK_PROC=5
N_SIMPLE=0 N_PIPE=1 N_AND=2 N_OR=3 N_NOT=4 N_LIST=5 N_IF=6 N_WHILE=7 N_UNTIL=8 N_FOR=9 N_CFOR=10
N_CASE=11 N_SUB=12 N_GROUP=13 N_FUNC=14 N_DBL=15 N_ARITH=16 N_BG=17

# ---- character classes used as patterns (unquoted: [[ $c == $WSP ]]) ----
WSP=$'[ \t\n]'                 # arithmetic whitespace
CSPACE=$'[ \t\n\v\f\r]'        # isspace()
NB=$'*[! \t]*'                 # has a character other than blank: s[strspn(s, " \t")] != 0
NBNL=$'*[! \t\n]*'
NOTNL=$'[!\n]'
NONPRINT='[! -~]'
IDNOT='[!a-zA-Z0-9$._-]'       # not an LLVM identifier character
DELIMS=$' \t\n|&;()<>'
WSTOP=$' \t\n|&;()<>'

die() { printf 'bashful: %s\n' "$1" >&2; exit 1; }

# ---- whole-script fallback: anything this compiler cannot translate exactly runs on an embedded bash ----
UNSUP= ARMED=0 UNSUP_FILE=
unsupported() { [[ -n $UNSUP ]] || UNSUP=$1; }
fb_abort() {   # a parse error while armed: give up on native compilation (we run in a subshell)
	[[ -n $UNSUP_FILE ]] && printf '%s' "$UNSUP" > "$UNSUP_FILE"
	exit 3
}

# ---- tools known to the runtime (single source of truth: runtime/tools.def) ----
declare -A TOOLIX
NTOOLS=0 NO_EMBED=0 CMD_MODE=0
load_tools() {
	local line kind id name
	while IFS= read -r line || [[ -n $line ]]; do
		case $line in INTERNAL\(* | EXTERNAL\(*) ;; *) continue ;; esac
		kind=${line%%(*}
		id=${line#*(}
		id=${id%%,*}
		name=${line#*\"}
		name=${name%\"*}
		TOOL_ID[NTOOLS]=$id
		TOOL_NAME[NTOOLS]=$name
		if [[ $kind == EXTERNAL ]]; then TOOL_PFX[NTOOLS]=bash_external_; else TOOL_PFX[NTOOLS]=bash_internal_; fi
		[[ -n ${TOOLIX[_$name]} ]] || TOOLIX[_$name]=$NTOOLS
		((NTOOLS++))
	done < "$RUNTIME_DIR/tools.def"
}
# R = tool index, -1 when not built in -- or an embedded program and --no-embed asked for the real one
tool_index() {
	R=${TOOLIX[_$1]}
	if [[ -z $R ]]; then R=-1; return; fi
	if ((NO_EMBED)) && [[ ${TOOL_PFX[R]} == bash_external_ ]]; then R=-1; fi
}

# R = s as the body of an LLVM c"..." constant (no terminating \00)
irbytes() {
	local s=$1 c pre h bs='\5C' dq='\22'
	if [[ $s != *$NONPRINT* && $s != *'"'* && $s != *'\'* ]]; then R=$s; return; fi
	s=${s//\\/"$bs"}
	s=${s//\"/"$dq"}
	while [[ $s == *$NONPRINT* ]]; do
		pre=${s%%$NONPRINT*}
		c=${s:${#pre}:1}
		printf -v h '\\%02X' "'$c"
		s=${s//"$c"/"$h"}
	done
	R=$s
}

# R = ord(c) of a one-byte string
ord() { printf -v R '%d' "'$1"; ((R < 0)) && R=$((R + 256)); }
# R = the byte with value v (0..255)
chr() { local o; printf -v o '%03o' $(($1 & 255)); printf -v R "\\$o"; }

# R = 16 hex digits, FNV-1a over the length and a sample of the bytes of $1.  Only a cache *key*: every
# cache hit is confirmed by comparing the full contents, so sampling can never return a wrong entry.
shash() {
	local s=$1 n=${#1} h=1469598103934665603 i step=1 c
	((n > 1024)) && step=$((n / 1024))
	h=$(((h ^ n) * 1099511628211))
	for ((i = 0; i < n; i += step)); do
		printf -v c '%d' "'${s:i:1}"
		h=$(((h ^ c) * 1099511628211))
	done
	for ((i = n > 32 ? n - 32 : 0; i < n; i++)); do
		printf -v c '%d' "'${s:i:1}"
		h=$(((h ^ c) * 1099511628211))
	done
	printf -v R '%016x' "$h"
}

# strtol(s, &end, 10) that must consume all of s: R = value
strtol_full() {
	local s=$1 neg=0
	while [[ $s == $CSPACE* ]]; do s=${s:1}; done
	if [[ $s == [+-]* ]]; then [[ $s == -* ]] && neg=1; s=${s:1}; fi
	[[ -n $s && $s != *[!0-9]* ]] || return 1
	c_strtol "$s" 10
	((neg)) && R=$((-R))
	return 0
}
# atoi(s): R = value of the leading integer (0 if none)
c_atoi() { c_strtol "$1" 10 || R=0; }
# strtol(s, &end, base) with clamping: R = value, SL = characters consumed (0: no number)
LONG_MAX=9223372036854775807
c_strtol() {
	local s=$1 b=$2 i=0 neg=0 v=0 d c st ovf=0 lim digs=0123456789abcdefghijklmnopqrstuvwxyz t
	while [[ ${s:i:1} == $CSPACE ]]; do ((i++)); done
	c=${s:i:1}
	if [[ $c == [+-] ]]; then [[ $c == - ]] && neg=1; ((i++)); fi
	if ((b == 0)); then
		if [[ ${s:i:1} == 0 ]]; then
			if [[ ${s:i+1:1} == [xX] && ${s:i+2:1} == [0-9a-fA-F] ]]; then b=16; ((i += 2)); else b=8; fi
		else b=10; fi
	elif ((b == 16)) && [[ ${s:i:1} == 0 && ${s:i+1:1} == [xX] && ${s:i+2:1} == [0-9a-fA-F] ]]; then
		((i += 2))
	fi
	st=$i
	lim=$((LONG_MAX / b))
	while c=${s:i:1}; [[ -n $c ]]; do
		case $c in
		[0-9]) d=$((10#$c)) ;;
		[a-z]) t=${digs%%"$c"*}; d=${#t} ;;
		[A-Z]) c=${c,}; t=${digs%%"$c"*}; d=${#t} ;;
		*) break ;;
		esac
		((d < b)) || break
		if ((v > lim || (v == lim && d > LONG_MAX % b))); then ovf=1; else v=$((v * b + d)); fi
		((i++))
	done
	if ((i == st)); then SL=0; R=0; return 1; fi
	if ((ovf)); then if ((neg)); then v=$((-LONG_MAX - 1)); else v=$LONG_MAX; fi
	elif ((neg)); then v=$((-v)); fi
	R=$v SL=$i
	return 0
}
