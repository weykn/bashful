# readline.sh - the line editor of the interactive shell (bashful -i), in the spirit of GNU readline in bash:
# emacs key bindings, a kill buffer, history (a file, ^R search, !-expansion), tab completion of commands, files,
# variables and ~users, bracketed paste and bash's prompt escapes (\u \h \w \$ \[ \] ...).  It is part of the
# compiler and compiled with it; only the terminal itself is C (__bashful_term in runtime/tool_term.c: raw mode,
# one key press, the window's width and the width of text on screen).
#
#   rl_readline 1|2            read a line with prompt PS1 / PS2 into REPLY; status 1 at end of input, 130 for ^C
#   rl_history add TEXT        remember a command (several lines are joined like bash's cmdhist does)
#   rl_history expand LINE     !-expansion into REPLY; status 0 unchanged, 1 expanded, 2 error, 3 expanded
#                              with :p (print, don't run)
#   rl_history list [N] | clear
#
# The REPL sets RL_STATE to the session's state prelude (the variables, PS1/PS2, HISTFILE, HISTSIZE,
# HISTFILESIZE, HISTCONTROL and PATH come from there) and RL_FUNCS to its function and alias names.

RL_STATE= RL_FUNCS= RL_SCANNED=$'\1' RL_KILL= RL_PEND=
RL_HLOADED=0 RL_HFILE= RL_HSIZE=500 RL_HFSIZE=500
RL_USER= RL_EUID= RL_TTY=
RL_E=$'\e' RL_CTL=$'[\001-\037\177]' RL_MARKS=$'[\001\002]' RL_CARET='@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\]^_'
RL_QCHARS=$' \t\n\\\'"`$&|;()<>!*?[]{}^'   # characters a completed word quotes
# keys: a byte's value, or one of these
K_EOF=-2 K_NONE=1000 K_UP=1001 K_DOWN=1002 K_LEFT=1003 K_RIGHT=1004 K_HOME=1005 K_END=1006 K_DEL=1007
K_WLEFT=1008 K_WRIGHT=1009 K_PASTE=1010 K_PGUP=1011 K_PGDN=1012 K_META=2000
declare -A RL_SV=()
RL_HIST=() RL_SNAMES=()
RL_KEYWORDS=(if then else elif fi case esac for select while until do done in function time '{' '}' '!' '[[' ']]' coproc)
RL_BUILTINS=(alias bg bind break builtin caller cd command compgen complete compopt continue declare dirs disown echo
	enable eval exec exit export false fc fg getopts hash help history jobs kill let local logout mapfile popd printf
	pushd pwd read readarray readonly return set shift shopt source suspend test times trap true type typeset ulimit
	umask unalias unset wait . : '[')

rl_isnum() { [[ -n $1 && $1 != *[!0-9]* ]]; }
rl_in() { [[ -n $2 && $1 == *"$2"* ]]; }   # chars char: is char one of chars

# ------------------------------------------------------------------ state text
# one shell word as the state prelude writes it ('...', $'...', "...", \x, plain) -> R, decoded
rl_unquote() {   # text
	local s=$1 i=0 n=${#1} c out= j t
	while ((i < n)); do
		c=${s:i:1}
		if [[ $c == "'" ]]; then
			j=$((i + 1))
			while ((j < n)) && [[ ${s:j:1} != "'" ]]; do ((j++)); done
			out+=${s:i+1:j-i-1}
			i=$((j + 1))
		elif [[ $c == '$' && ${s:i+1:1} == "'" ]]; then
			j=$((i + 2))
			while ((j < n)) && [[ ${s:j:1} != "'" ]]; do [[ ${s:j:1} == '\' ]] && ((j++)); ((j++)); done
			t=${s:i+2:j-i-2}
			out+=${t@E}
			i=$((j + 1))
		elif [[ $c == '"' ]]; then
			((i++))
			while ((i < n)) && [[ ${s:i:1} != '"' ]]; do
				if [[ ${s:i:1} == '\' && ${s:i+1:1} == [\"\\\$\`] ]]; then ((i++)); fi
				out+=${s:i:1}
				((i++))
			done
			((i++))
		elif [[ $c == '\' ]] && ((i + 1 < n)); then out+=${s:i+1:1}; ((i += 2))
		else out+=$c; ((i++)); fi
	done
	R=$out
}
# RL_STATE -> RL_SV[name] (the scalar values) and RL_SNAMES (every variable it sets); only when it changed
rl_state_scan() {
	local line name val q pend=
	[[ $RL_STATE == "$RL_SCANNED" ]] && return 0
	RL_SCANNED=$RL_STATE
	RL_SV=() RL_SNAMES=()
	while IFS= read -r line; do
		if [[ -n $pend ]]; then   # a '...' value with newlines in it (the C state dump writes them as they are)
			pend+=$'\n'$line
			q=${pend//[!\']/}
			((${#q} % 2)) && continue
			line=$pend pend=
		fi
		[[ $line == [a-zA-Z_]* ]] || continue
		name=${line%%[=[]*}
		[[ $name == *[!a-zA-Z0-9_]* || $name == "$line" ]] && continue
		RL_SNAMES+=("$name")
		[[ ${line:${#name}:1} == '=' ]] || continue
		val=${line#*=}
		if [[ $val == "'"* ]]; then
			q=${val//[!\']/}
			if ((${#q} % 2)); then pend=$line; continue; fi
		fi
		rl_unquote "$val"
		RL_SV[_$name]=$R
	done <<< "$RL_STATE"
	return 0
}
rl_var() {   # name -> R = its value in the session, status 1 when unset
	rl_state_scan
	[[ -n ${RL_SV[_$1]+x} ]] || { R=; return 1; }
	R=${RL_SV[_$1]}
}

# --------------------------------------------------------------------- history
# like bash: HISTFILE (unset or empty: no history file), HISTSIZE entries kept, the file cut down to
# HISTFILESIZE lines; all three are the session's variables (the REPL starts them at bash's defaults)
rl_hist_num() {   # name default -> R
	rl_var "$1" || { R=$2; return; }
	if rl_isnum "${R#-}"; then
		if ((R < 0 || R > 1000000)); then R=1000000; fi
	else R=$2; fi
}
rl_hist_settings() {
	rl_var HISTFILE
	RL_HFILE=$R
	rl_hist_num HISTSIZE 500
	RL_HSIZE=$R
	rl_hist_num HISTFILESIZE $RL_HSIZE
	RL_HFSIZE=$R
}
rl_hist_push() {   # entry
	((RL_HSIZE > 0)) || return 0
	RL_HIST+=("$1")
	((${#RL_HIST[@]} > RL_HSIZE)) && RL_HIST=("${RL_HIST[@]:${#RL_HIST[@]}-RL_HSIZE}")
	return 0
}
rl_hist_load() {
	local -a all lines
	local l tmp
	((RL_HLOADED)) && return 0
	RL_HLOADED=1
	rl_hist_settings
	[[ -n $RL_HFILE && -r $RL_HFILE ]] || return 0
	mapfile -t lines < "$RL_HFILE"
	for l in "${lines[@]}"; do
		[[ -z $l ]] && continue
		[[ $l == '#'* ]] && rl_isnum "${l:1}" && continue   # (timestamps of HISTTIMEFORMAT)
		all+=("$l")
	done
	if ((${#all[@]} > RL_HSIZE)); then RL_HIST=("${all[@]:${#all[@]}-RL_HSIZE}"); else RL_HIST=("${all[@]}"); fi
	((RL_HSIZE > 0)) || RL_HIST=()
	# bash cuts the file down to HISTFILESIZE lines (when it exits); here when the session starts, with some
	# slack so that it is not rewritten every time
	if ((${#all[@]} > RL_HFSIZE + RL_HFSIZE / 4 + 16)); then
		tmp=$RL_HFILE.bashful$$
		if printf '%s\n' "${all[@]:${#all[@]}-RL_HFSIZE}" > "$tmp"; then mv -f "$tmp" "$RL_HFILE" 2> /dev/null || rm -f "$tmp"; fi
	fi
	return 0
}
# several input lines -> one history entry -> R, like bash with cmdhist: "; " between commands, a blank after
# words that continue a command, and the newline kept where joining could change the meaning
rl_join_lines() {   # text
	local t=$1 o= q= keep=0 comment=0 L i c k sp w
	[[ $t == *'<<'* || $t == *case* ]] && keep=1
	while [[ $t == *$'\n' ]]; do t=${t%$'\n'}; done
	L=${#t}
	for ((i = 0; i < L; i++)); do
		c=${t:i:1}
		if [[ $q == "'" ]]; then [[ $c == "'" ]] && q=; o+=$c; continue; fi
		if [[ $c == '\' ]] && ((i + 1 < L)); then
			if [[ ${t:i+1:1} == $'\n' && -z $q ]]; then ((i++)); continue; fi   # line continuation
			o+=$c${t:i+1:1}
			((i++))
			continue
		fi
		if [[ $q == '"' ]]; then [[ $c == '"' ]] && q=; o+=$c; continue; fi
		if [[ $c == "'" || $c == '"' ]]; then q=$c; o+=$c; continue; fi
		[[ $c == '#' ]] && { [[ -z $o ]] || rl_in $' \t\n;|&(' "${o: -1}"; } && comment=1
		if [[ $c != $'\n' ]]; then o+=$c; continue; fi
		if ((keep || comment)); then o+=$'\n' comment=0; continue; fi
		while [[ $o == *[' '$'\t'] ]]; do o=${o%?}; done
		sp=0
		for w in then do else '{' '(' '|' '&&' '||' in ';' '()'; do
			[[ $o == *"$w" ]] || continue
			k=$((${#o} - ${#w}))
			if ((k == 0)) || [[ $w != [a-z]* ]] || rl_in $' \t;|&' "${o:k-1:1}"; then sp=1; break; fi
		done
		[[ -z $o ]] && continue
		if ((sp)); then o+=' '; else o+='; '; fi
		while ((i + 1 < L)) && [[ ${t:i+1:1} == [' '$'\t'] ]]; do ((i++)); done
	done
	R=$o
}
rl_hist_add() {   # text
	local e hc n
	rl_hist_load
	rl_hist_settings
	rl_var HISTCONTROL
	hc=$R
	rl_join_lines "$1"
	e=$R
	[[ -n $e ]] || return 0
	[[ $hc == *ignorespace* || $hc == *ignoreboth* ]] && [[ $e == ' '* ]] && return 0
	n=${#RL_HIST[@]}
	[[ $hc == *ignoredups* || $hc == *ignoreboth* ]] && ((n)) && [[ ${RL_HIST[n - 1]} == "$e" ]] && return 0
	rl_hist_push "$e"
	[[ -n $RL_HFILE ]] && printf '%s\n' "$e" >> "$RL_HFILE" 2> /dev/null
	return 0
}
# the words of a history entry for !-designators -> RL_W: blanks separate, quotes group, ; | & ( ) < > are words
rl_hwords() {   # line
	local s=$1 i=0 n=${#1} c w q
	RL_W=()
	while ((i < n)); do
		while ((i < n)) && [[ ${s:i:1} == [$' \t\n'] ]]; do ((i++)); done
		((i < n)) || break
		c=${s:i:1}
		if [[ $c == [\;\|\&\(\)\<\>] ]]; then
			w=$c
			((i++))
			[[ $c == [\|\&\<\>\;] && ${s:i:1} == "$c" ]] && { w+=$c; ((i++)); }
			RL_W+=("$w")
			continue
		fi
		w= q=
		while ((i < n)); do
			c=${s:i:1}
			[[ -z $q ]] && rl_in $' \t\n;|&()<>' "$c" && break
			if [[ -n $q ]]; then
				if [[ $c == "$q" ]]; then q=
				elif [[ $q == '"' && $c == '\' ]] && ((i + 1 < n)); then w+=$c; ((i++)); c=${s:i:1}; fi
			elif [[ $c == "'" || $c == '"' ]]; then q=$c
			elif [[ $c == '\' ]] && ((i + 1 < n)); then w+=$c; ((i++)); c=${s:i:1}; fi
			w+=$c
			((i++))
		done
		RL_W+=("$w")
	done
}
rl_herr() { printf 'bashful: %s: %s\n' "$1" "$2" >&2; }
rl_num_at() {   # text index -> R = the number there, RL_J = the index after it
	local s=$1 j=$2
	R=0
	while [[ ${s:j:1} == [0-9] ]]; do R=$((R * 10 + ${s:j:1})); ((j++)); done
	RL_J=$j
}
# !-expansion of one input line -> R; status 0 unchanged, 1 expanded, 2 error (message printed), 3 with :p
rl_hist_expand() {   # line
	local in=$1 n=${#1} o= changed=0 ponly=0 q= i j c nx ev wd s pat k line piece last a b d m old new prev rest
	local hn
	rl_hist_load
	hn=${#RL_HIST[@]}
	if [[ $in == '^'?* ]]; then   # ^old^new^
		((hn)) || { rl_herr "$in" "event not found"; return 2; }
		rest=${in:1}
		old=${rest%%^*}
		new=
		if [[ $rest == *^* ]]; then new=${rest#*^}; rest=${new#*^}; [[ $new == *^* ]] || rest=; new=${new%%^*}; else rest=; fi
		prev=${RL_HIST[hn - 1]}
		if [[ -z $old || $prev != *"$old"* ]]; then printf 'bashful: :s%s: substitution failed\n' "$in" >&2; return 2; fi
		R=${prev/"$old"/"$new"}$rest
		return 1
	fi
	for ((i = 0; i < n; i++)); do
		c=${in:i:1}
		if [[ $q == "'" ]]; then [[ $c == "'" ]] && q=; o+=$c; continue; fi
		if [[ $c == '\' ]] && ((i + 1 < n)); then o+=$c${in:i+1:1}; ((i++)); continue; fi
		if [[ $c == "'" && -z $q ]]; then q=$c; o+=$c; continue; fi
		if [[ $c == '"' ]]; then if [[ -n $q ]]; then q=; else q='"'; fi; o+=$c; continue; fi
		nx=${in:i+1:1}
		if [[ $c != '!' || -z $nx ]] || rl_in $' \t\n=(' "$nx" || [[ $q == '"' && $nx == '"' ]] || { ((i)) && rl_in '${[' "${in:i-1:1}"; }; then
			o+=$c
			continue
		fi
		j=$((i + 1)) ev=-1 wd=0
		if [[ $nx == '!' ]]; then ev=$((hn - 1)) j=$((i + 2))
		elif [[ $nx == [0-9] ]]; then rl_num_at "$in" $j; ev=$((R - 1)) j=$RL_J
		elif [[ $nx == - && ${in:i+2:1} == [0-9] ]]; then rl_num_at "$in" $((i + 2)); ev=$((hn - R)) j=$RL_J
		elif [[ $nx == [\$^*] ]]; then ev=$((hn - 1)) wd=1
		elif [[ $nx == '?' ]]; then
			s=$((i + 2)) j=$s
			while ((j < n)) && ! rl_in $'?\n' "${in:j:1}"; do ((j++)); done
			pat=${in:s:j-s}
			[[ ${in:j:1} == '?' ]] && ((j++))
			ev=-2
			for ((k = hn - 1; k >= 0; k--)); do [[ ${RL_HIST[k]} == *"$pat"* ]] && { ev=$k; break; }; done
		elif [[ $nx == '#' ]]; then o+=$c; continue
		else
			s=$((i + 1)) j=$s
			while ((j < n)) && ! rl_in $' \t\n:;&|()<>"\'' "${in:j:1}"; do ((j++)); done
			pat=${in:s:j-s}
			ev=-2
			for ((k = hn - 1; k >= 0; k--)); do [[ ${RL_HIST[k]} == "$pat"* ]] && { ev=$k; break; }; done
		fi
		if ((ev < 0 || ev >= hn)); then rl_herr "${in:i:j-i}" "event not found"; return 2; fi
		line=${RL_HIST[ev]}
		if ((wd)) || [[ ${in:j:1} == : && ${in:j+1:1} == [0-9^\$*-] ]]; then
			((wd)) || ((j++))
			rl_hwords "$line"
			last=$((${#RL_W[@]} - 1))
			d=${in:j:1}
			if [[ $d == '^' ]]; then a=1 b=1; ((j++))
			elif [[ $d == '$' ]]; then a=$last b=$last; ((j++))
			elif [[ $d == '*' ]]; then a=1 b=$last; ((j++))
			else
				a=0
				if [[ $d == [0-9] ]]; then rl_num_at "$in" $j; a=$R j=$RL_J; fi
				b=$a
				if [[ ${in:j:1} == '*' ]]; then b=$last; ((j++))
				elif [[ ${in:j:1} == - ]]; then
					((j++))
					if [[ ${in:j:1} == '$' ]]; then b=$last; ((j++))
					elif [[ ${in:j:1} == [0-9] ]]; then rl_num_at "$in" $j; b=$R j=$RL_J
					else b=$((last - 1)); fi
				fi
			fi
			if ((a < 0 || a > last || b > last)) || { ((b < a)) && { [[ $d != '*' ]] || ((last >= 1)); }; }; then
				rl_herr "${in:i:j-i}" "bad word specifier"
				return 2
			fi
			piece=
			for ((k = a; k <= b; k++)); do ((k > a)) && piece+=' '; piece+=${RL_W[k]}; done
		else piece=$line; fi
		while [[ ${in:j:1} == : && ${in:j+1:1} == [htrep] ]]; do   # modifiers
			m=${in:j+1:1}
			((j += 2))
			case $m in
			p) ponly=1 ;;
			h) [[ $piece == */* ]] && piece=${piece%/*} ;;
			t) [[ $piece == */* ]] && piece=${piece##*/} ;;
			r) rest=${piece##*/}; [[ $rest == *.* ]] && piece=${piece%.*} ;;
			e) rest=${piece##*/}; if [[ $rest == *.* ]]; then piece=.${rest##*.}; else piece=; fi ;;
			esac
		done
		o+=$piece
		changed=1
		i=$((j - 1))
	done
	R=$o
	((changed)) || return 0
	((ponly)) && return 3
	return 1
}
rl_history() {   # add TEXT | expand LINE | list [N] | clear
	local n i
	case $1 in
	add) rl_hist_add "$2" ;;
	expand)
		rl_hist_expand "$2"
		n=$?
		if ((n)); then REPLY=$R; else REPLY=$2; fi
		return $n
		;;
	list)
		rl_hist_load
		n=${#RL_HIST[@]}
		if [[ -n $2 ]]; then
			if ! rl_isnum "$2"; then printf 'bashful: history: %s: numeric argument required\n' "$2" >&2; return 2; fi
			((10#$2 < n)) && n=$((10#$2))
		fi
		for ((i = ${#RL_HIST[@]} - n; i < ${#RL_HIST[@]}; i++)); do printf '%5d  %s\n' $((i + 1)) "${RL_HIST[i]}"; done
		;;
	clear)
		rl_hist_load
		RL_HIST=()
		[[ -n $RL_HFILE && -e $RL_HFILE ]] && : > "$RL_HFILE"
		;;
	*) return 2 ;;
	esac
	return 0
}

# --------------------------------------------------------------------- prompts
rl_who() {   # the user name, the effective uid and the terminal, once
	local k a b rest
	[[ -n $RL_EUID ]] && return 0
	RL_USER=$(whoami 2> /dev/null)
	RL_EUID=0
	while read -r k a b rest; do [[ $k == Uid: ]] && { RL_EUID=$b; break; }; done < /proc/self/status
	return 0
}
# bash's prompt escapes -> R; \[ \] become \1 \2 (not counted in the prompt's width)
rl_prompt_expand() {   # prompt
	local p=$1 n=${#1} i c o= t h d
	for ((i = 0; i < n; i++)); do
		c=${p:i:1}
		if [[ $c != '\' ]] || ((i + 1 >= n)); then o+=$c; continue; fi
		((i++))
		c=${p:i:1}
		case $c in
		a) o+=$'\a' ;;
		e) o+=$RL_E ;;
		n) o+=$'\n' ;;
		r) o+=$'\r' ;;
		'\') o+='\' ;;
		'[') o+=$'\1' ;;
		']') o+=$'\2' ;;
		s) o+=bashful ;;
		v | V) o+=5.2 ;;
		j) o+=0 ;;
		'!' | '#') o+=$((${#RL_HIST[@]} + 1)) ;;
		d) printf -v t '%(%a %b %d)T' -1; o+=$t ;;
		t) printf -v t '%(%H:%M:%S)T' -1; o+=$t ;;
		T) printf -v t '%(%I:%M:%S)T' -1; o+=$t ;;
		'@') printf -v t '%(%I:%M %p)T' -1; o+=$t ;;
		A) printf -v t '%(%H:%M)T' -1; o+=$t ;;
		D)
			if [[ ${p:i+1:1} == '{' && ${p:i+2} == *'}'* ]]; then
				t=${p:i+2}
				t=${t%%'}'*}
				i=$((i + 2 + ${#t}))
				[[ -n $t ]] || t=%X
				printf -v t "%($t)T" -1
				o+=$t
			else o+='\D'; fi
			;;
		u) rl_who; o+=$RL_USER ;;
		h | H)
			rl_var HOSTNAME || { h=; [[ -r /proc/sys/kernel/hostname ]] && read -r h < /proc/sys/kernel/hostname; R=$h; }
			h=$R
			[[ $c == h ]] && h=${h%%.*}
			o+=$h
			;;
		l)
			[[ -n $RL_TTY ]] || { RL_TTY=$(tty 2> /dev/null); RL_TTY=${RL_TTY##*/}; [[ -n $RL_TTY ]] || RL_TTY=tty; }
			o+=$RL_TTY
			;;
		'$') rl_who; if ((RL_EUID == 0)); then o+='#'; else o+='$'; fi ;;
		w | W)
			rl_var HOME || R=$HOME
			h=$R d=$PWD
			if [[ $c == W ]]; then
				if ((${#h} > 1)) && [[ $d == "$h" ]]; then o+='~'
				elif [[ $d == / ]]; then o+=/
				else o+=${d##*/}; fi
			elif ((${#h} > 1)) && [[ $d == "$h" || $d == "$h"/* ]]; then o+="~${d:${#h}}"
			else o+=$d; fi
			;;
		[0-7])
			t=$c
			while ((${#t} < 3)) && [[ ${p:i+1:1} == [0-7] ]]; do ((i++)); t+=${p:i:1}; done
			printf -v t "\\$t"
			o+=$t
			;;
		*) o+='\'$c ;;
		esac
	done
	R=$o
}
rl_pwidth() {   # prompt line -> R = its width on screen (\1...\2 and control characters excluded)
	local s=$1 v=
	while [[ $s == *$'\1'* ]]; do
		v+=${s%%$'\1'*}
		s=${s#*$'\1'}
		[[ $s == *$'\2'* ]] && s=${s#*$'\2'} || s=
	done
	v+=$s
	__bashful_term width "$v" 0
	R=$REPLY
}

# ---------------------------------------------------------------------- screen
rl_render() {   # text -> R, control characters shown as ^X
	local s=$1 i c n o=
	if [[ $s != *$RL_CTL* ]]; then R=$s; return; fi
	for ((i = 0; i < ${#s}; i++)); do
		c=${s:i:1}
		if [[ $c == $RL_CTL ]]; then
			printf -v n %d "'$c"
			if ((n == 127)); then o+='^?'; else o+="^${RL_CARET:n:1}"; fi
		else o+=$c; fi
	done
	R=$o
}
rl_cols() { __bashful_term cols; RL_COLS=$REPLY; }
# redraw the line: the prompt's last line, the text, the cursor at RL_P (rows counted from RL_ROW)
rl_refresh() {
	local o= W C er tr tc
	rl_cols
	((RL_ROW > 0)) && o+="$RL_E[${RL_ROW}A"
	o+=$'\r'"$RL_E[J"
	o+=${RL_PLINE//$RL_MARKS/}
	rl_render "$RL_B"
	o+=$R
	__bashful_term width "$RL_B"
	W=$((RL_PW + REPLY))
	__bashful_term width "${RL_B:0:RL_P}"
	C=$((RL_PW + REPLY))
	((W > 0 && W % RL_COLS == 0)) && o+=$'\r\n'   # leave the pending-wrap state at the right margin
	er=$((W / RL_COLS)) tr=$((C / RL_COLS)) tc=$((C % RL_COLS))
	((er > tr)) && o+="$RL_E[$((er - tr))A"
	o+=$'\r'
	((tc)) && o+="$RL_E[${tc}C"
	RL_ROW=$tr
	printf '%s' "$o" >&2
}
rl_to_end() { RL_P=${#RL_B}; rl_refresh; }
rl_full_prompt() { printf '%s' "${RL_PFIRST//$RL_MARKS/}" >&2; RL_ROW=0; }

# ------------------------------------------------------------------------ keys
# one key -> RK: a byte's value, K_* for escape sequences, K_META + byte for Alt-x; RL_KEY = its text
rl_readkey() {
	local st par f
	__bashful_term key
	st=$?
	if ((st == 2)); then RK=$K_EOF; return; fi
	RL_KEY=$KEY RK=$KEYN
	((RK == 27)) || return 0
	__bashful_term key 50 || { RK=27; return; }
	if [[ $KEY == '[' || $KEY == O ]]; then
		par=
		while :; do
			__bashful_term key 50 || { RK=$K_NONE; return; }
			f=$KEY
			((KEYN >= 0x40 && KEYN <= 0x7e)) && break
			par+=$f
		done
		RK=$K_NONE
		case $f in
		A) RK=$K_UP ;;
		B) RK=$K_DOWN ;;
		C) if [[ $par == *';'* ]]; then RK=$K_WRIGHT; else RK=$K_RIGHT; fi ;;
		D) if [[ $par == *';'* ]]; then RK=$K_WLEFT; else RK=$K_LEFT; fi ;;
		H) RK=$K_HOME ;;
		F) RK=$K_END ;;
		'~')
			case ${par%%;*} in
			1 | 7) RK=$K_HOME ;;
			4 | 8) RK=$K_END ;;
			3) RK=$K_DEL ;;
			5) RK=$K_PGUP ;;
			6) RK=$K_PGDN ;;
			200) RK=$K_PASTE ;;
			esac
			;;
		esac
		return 0
	fi
	RK=$((K_META + KEYN))
}

# ------------------------------------------------------------------ completion
rl_is_assign() {   # word: NAME=... NAME+=... NAME[...
	local n=${1%%[=+[]*} r
	r=${1:${#n}}
	[[ $n == [a-zA-Z_]* && $n != *[!a-zA-Z0-9_]* ]] && [[ $r == =* || $r == +=* || $r == '['* ]]
}
rl_expand_tilde() {   # dir -> R, "~/x" "~user/x" with the real directory
	local d=$1 u rest l h=
	if [[ $d != '~'* ]]; then R=$d; return; fi
	u=${d#'~'}
	rest=
	[[ $u == */* ]] && rest=/${u#*/} u=${u%%/*}
	if [[ -z $u ]]; then rl_var HOME || R=$HOME; h=$R
	else
		while IFS= read -r l; do [[ ${l%%:*} == "$u" ]] && { h=${l#*:*:*:*:*:}; h=${h%%:*}; break; }; done < /etc/passwd
	fi
	if [[ -z $h ]]; then R=$d; else R=$h$rest; fi
}
# names in dir starting with base -> RL_M (directories with a trailing /)
rl_files() {   # dirpart base execs_only dirs_only
	local d f nm
	rl_expand_tilde "${1:-.}"
	d=$R
	for f in "${d%/}/$2"*; do
		[[ -e $f || -L $f ]] || continue
		nm=${f##*/}
		[[ $nm == . || $nm == .. ]] && [[ $2 != .* ]] && continue
		if [[ -d $f ]]; then RL_M+=("$nm/")
		elif (($4)); then continue
		elif (($3)) && ! [[ -f $f && -x $f ]]; then continue
		else RL_M+=("$nm"); fi
	done
}
rl_commands() {   # base -> RL_M: keywords, builtins, functions, aliases, programs in PATH, directories
	local b=$1 k path dir f
	for k in "${RL_KEYWORDS[@]}" "${RL_BUILTINS[@]}" $RL_FUNCS; do [[ $k == "$b"* ]] && RL_M+=("$k"); done
	rl_var PATH || R=$PATH
	path=$R
	while [[ -n $path ]]; do
		dir=${path%%:*}
		[[ $path == *:* ]] && path=${path#*:} || path=
		for f in "${dir:-.}/$b"*; do
			[[ -f $f && -x $f ]] && RL_M+=("${f##*/}")
		done
	done
	rl_files "" "$b" 0 1
}
rl_quote() {   # text quote at_start -> R, quoted for the word being completed
	local s=$1 q=$2 i c o=
	for ((i = 0; i < ${#s}; i++)); do
		c=${s:i:1}
		if [[ $q == "'" ]]; then if [[ $c == "'" ]]; then o+="'\\''"; else o+=$c; fi; continue; fi
		if [[ $q == '"' ]]; then [[ $c == [\"\$\`\\] ]] && o+='\'; o+=$c; continue; fi
		if rl_in "$RL_QCHARS" "$c" || { (($3)) && ((i == 0)) && rl_in '#~=' "$c"; }; then o+='\'; fi
		o+=$c
	done
	R=$o
}
rl_ask_yn() {
	while :; do
		rl_readkey
		case $RK in
		121 | 89 | 32) return 0 ;;   # y Y space
		110 | 78 | 127 | 3 | 7 | $K_EOF) return 1 ;;
		esac
	done
}
rl_list_matches() {
	local cols maxw=0 per rows r c i w o= k
	local -a ws
	rl_to_end
	printf '\r\n' >&2
	rl_cols
	if ((${#RL_M[@]} >= 100)); then
		printf 'Display all %d possibilities? (y or n)' ${#RL_M[@]} >&2
		if ! rl_ask_yn; then printf '\r\n' >&2; rl_full_prompt; rl_refresh; return; fi
		printf '\r\n' >&2
	fi
	for ((i = 0; i < ${#RL_M[@]}; i++)); do
		__bashful_term width "${RL_M[i]}"
		ws[i]=$REPLY
		((REPLY > maxw)) && maxw=$REPLY
	done
	per=$((RL_COLS / (maxw + 2)))
	((per < 1)) && per=1
	rows=$(((${#RL_M[@]} + per - 1) / per))
	for ((r = 0; r < rows; r++)); do
		for ((c = 0; c < per; c++)); do
			i=$((c * rows + r))
			((i < ${#RL_M[@]})) || break
			rl_render "${RL_M[i]}"
			o+=$R
			if (((c + 1) * rows + r < ${#RL_M[@]})); then for ((k = ws[i]; k < maxw + 2; k++)); do o+=' '; done; fi
		done
		o+=$'\r\n'
	done
	printf '%s' "$o" >&2
	rl_full_prompt
	rl_refresh
}
# Tab: complete the word before the cursor (second: the second Tab in a row lists the choices)
rl_complete() {   # second
	local s=$RL_B pos=$RL_P cmdpos=1 redir=0 inword=0 q= ws i c t k digits brk sep rd
	local u= un=0 qend mode bstart=0 dl=-1 dollar brace=0 base lcp bl w wl isdir rs qb ins dstart dirpart x
	local -a from qs
	ws=$pos
	# the word under the cursor, and what kind of word it is
	for ((i = 0; i < pos; i++)); do
		c=${s:i:1}
		if [[ $q == "'" ]]; then [[ $c == "'" ]] && q=; continue; fi
		if [[ $q == '"' ]]; then if [[ $c == '\' ]]; then ((i++)); elif [[ $c == '"' ]]; then q=; fi; continue; fi
		if [[ $c == '\' ]]; then ((inword)) || { inword=1; ws=$i; }; ((i++)); continue; fi
		if [[ $c == "'" || $c == '"' ]]; then ((inword)) || { inword=1; ws=$i; }; q=$c; continue; fi
		brk=0 sep=0 rd=0
		[[ $c == [$' \t\n'] ]] && brk=1
		[[ $c == [\;\|\&\(\)] ]] && sep=1
		[[ $c == [\<\>] ]] && rd=1
		if ((brk || sep || rd)); then
			if ((inword)); then
				t=${s:ws:i-ws}
				digits=0
				rl_isnum "$t" && digits=1
				if ((rd && digits)); then :
				elif ((redir)); then redir=0
				elif ((cmdpos)); then
					k=0
					for x in "${RL_KEYWORDS[@]}"; do [[ $x == "$t" ]] && { k=1; break; }; done
					((k)) || rl_is_assign "$t" || cmdpos=0
				fi
				inword=0
			fi
			((sep)) && cmdpos=1 redir=0
			((rd)) && redir=1
			continue
		fi
		((inword)) || { inword=1; ws=$i; }
	done
	((inword)) || ws=$pos
	# unescape the word, remembering where each character came from and its quoting
	q=
	for ((i = ws; i < pos; i++)); do
		c=${s:i:1}
		if [[ $q == "'" ]]; then [[ $c == "'" ]] && { q=; continue; }
		elif [[ $q == '"' ]]; then
			[[ $c == '"' ]] && { q=; continue; }
			if [[ $c == '\' ]] && ((i + 1 < pos)) && [[ ${s:i+1:1} == [\"\$\`\\] ]]; then ((i++)); c=${s:i:1}; fi
		elif [[ $c == "'" || $c == '"' ]]; then q=$c; continue
		elif [[ $c == '\' ]] && ((i + 1 < pos)); then ((i++)); c=${s:i:1}; fi
		u+=$c from[un]=$i qs[un]=$q
		((un++))
	done
	from[un]=$pos qs[un]=$q
	qend=$q
	# the part being completed: mode 0 files, 1 commands, 2 variables, 3 users
	dollar=$un
	for ((i = un - 1; i >= 0; i--)); do
		c=${u:i:1}
		if [[ $c == '$' && ${qs[i]} != "'" ]]; then dollar=$i; break; fi
		[[ $c == [a-zA-Z0-9_{] ]] || break
	done
	if ((dollar < un)); then
		mode=2 bstart=$((dollar + 1))
		[[ ${u:bstart:1} == '{' ]] && { brace=1; ((bstart++)); }
	elif [[ $u == '~'* && $u != */* ]]; then mode=3 bstart=1
	else
		if ((!cmdpos || redir)); then
			if ((!redir)); then for ((i = 0; i < un; i++)); do [[ ${u:i:1} == = && -z ${qs[i]} ]] && bstart=$((i + 1)); done; fi
		elif rl_is_assign "$u"; then x=${u%%=*}; bstart=$((${#x} + 1)) cmdpos=0
		fi
		x=${u:bstart}
		if [[ $x == */* ]]; then x=${x%/*}; dl=$((bstart + ${#x})); fi
		mode=0
		((cmdpos && !redir && dl < 0)) && mode=1
		((dl >= 0)) && bstart=$((dl + 1))
	fi
	base=${u:bstart}
	RL_M=()
	if ((mode == 2)); then
		rl_state_scan
		for x in "${RL_SNAMES[@]}"; do [[ $x == "$base"* ]] && RL_M+=("$x"); done
	elif ((mode == 3)); then
		while IFS= read -r x; do x=${x%%:*}; [[ -n $x && $x == "$base"* ]] && RL_M+=("$x/"); done < /etc/passwd
	elif ((mode == 1)); then rl_commands "$base"
	else
		dstart=0
		if ((!redir)); then for ((i = 0; i < bstart; i++)); do [[ ${u:i:1} == = && -z ${qs[i]} ]] && dstart=$((i + 1)); done; fi
		((dl >= 0 && dstart > dl)) && dstart=0
		dirpart=${u:dstart:bstart-dstart}
		rl_files "$dirpart" "$base" $((cmdpos && !redir)) 0
	fi
	((${#RL_M[@]})) && mapfile -t RL_M < <(printf '%s\n' "${RL_M[@]}" | LC_ALL=C sort -u)
	if ((!${#RL_M[@]})); then printf '\a' >&2; return; fi
	# the longest common prefix
	lcp=${RL_M[0]}
	for x in "${RL_M[@]:1}"; do
		while [[ -n $lcp && $x != "$lcp"* ]]; do lcp=${lcp%?}; done
	done
	bl=${#base}
	if ((${#RL_M[@]} > 1 && ${#lcp} <= bl)); then
		if (($1)); then rl_list_matches; else printf '\a' >&2; fi
		return
	fi
	if ((${#RL_M[@]} == 1)); then w=${RL_M[0]}; else w=$lcp; fi
	isdir=0
	if ((${#RL_M[@]} == 1)) && [[ $w == */ ]]; then isdir=1 w=${w%/}; fi
	rs=${from[bstart]}   # the text from here to the cursor is replaced
	qb=${qs[bstart]}
	[[ -n $qb ]] || qb=$qend
	x=$qb
	((mode == 2 || mode == 3)) && x=
	rl_quote "$w" "$x" $((bstart == 0 && rs == ws))
	ins=$R
	if ((${#RL_M[@]} == 1)); then
		if ((isdir)); then ins+=/
		elif ((mode == 2)); then
			if ((brace)); then ins+='}'; elif [[ ${RL_B:pos:1} != ' ' ]]; then ins+=' '; fi
		else
			ins+=$qb
			[[ ${RL_B:pos:1} == ' ' ]] || ins+=' '
		fi
	fi
	RL_B=${RL_B:0:rs}$ins${RL_B:pos}
	RL_P=$((rs + ${#ins}))
}

# --------------------------------------------------------------------- editing
rl_insert() { RL_B=${RL_B:0:RL_P}$1${RL_B:RL_P}; RL_P=$((RL_P + ${#1})); }
rl_kill() {   # from to: cut into the kill buffer
	(($2 > $1)) || return 0
	RL_KILL=${RL_B:$1:$2-$1}
	RL_B=${RL_B:0:$1}${RL_B:$2}
	RL_P=$1
}
rl_isword() {   # char: letters, digits and anything not ASCII
	local n
	[[ $1 == [a-zA-Z0-9] ]] && return 0
	printf -v n %d "'$1"
	((n >= 128))
}
rl_word_left() {   # -> R
	local p=$RL_P
	while ((p > 0)) && ! rl_isword "${RL_B:p-1:1}"; do ((p--)); done
	while ((p > 0)) && rl_isword "${RL_B:p-1:1}"; do ((p--)); done
	R=$p
}
rl_word_right() {   # -> R
	local p=$RL_P n=${#RL_B}
	while ((p < n)) && ! rl_isword "${RL_B:p:1}"; do ((p++)); done
	while ((p < n)) && rl_isword "${RL_B:p:1}"; do ((p++)); done
	R=$p
}
rl_hist_go() {   # index: show that history entry (the end: the line being typed)
	local to=$1 hn=${#RL_HIST[@]}
	((to < 0 || to > hn || to == RL_HIDX)) && return 0
	((RL_HIDX == hn)) && RL_SAVED=$RL_B
	RL_HIDX=$to
	if ((to == hn)); then RL_B=$RL_SAVED; else RL_B=${RL_HIST[to]}; fi
	RL_P=${#RL_B}
}
# ^R: RK = the key that ended the search (-1: the line accepted with Enter, K_NONE: cancelled)
rl_rsearch() {
	local op=$RL_PLINE opw=$RL_PW orig=$RL_B opos=$RL_P q= at=$RL_HIDX fail=0 from k hn=${#RL_HIST[@]} f
	while :; do
		if ((fail)); then RL_PLINE="(failed reverse-i-search)\`$q': "; else RL_PLINE="(reverse-i-search)\`$q': "; fi
		rl_pwidth "$RL_PLINE"
		RL_PW=$R
		rl_refresh
		rl_readkey
		from=-1
		if ((RK == 18)); then from=$((at - 1))
		elif ((RK == 127 || RK == 8)); then q=${q%?}; from=$RL_HIDX at=$RL_HIDX
		elif ((RK == 7 || RK == 3)); then RL_B=$orig RL_P=$opos RK=$K_NONE; break
		elif ((RK >= 32 && RK < 256)); then q+=$RL_KEY; if ((at < hn)); then from=$at; else from=$((hn - 1)); fi
		else break; fi
		if [[ -z $q ]]; then fail=0; continue; fi
		fail=1
		((from > hn - 1)) && from=$((hn - 1))
		for ((k = from; k >= 0; k--)); do
			if [[ ${RL_HIST[k]} == *"$q"* ]]; then
				at=$k RL_B=${RL_HIST[k]} fail=0
				f=${RL_B%%"$q"*}
				RL_P=${#f}
				break
			fi
		done
		((fail)) && printf '\a' >&2
	done
	if ((at < hn && at != RL_HIDX)); then ((RL_HIDX == hn)) && RL_SAVED=$orig; RL_HIDX=$at; fi
	RL_PLINE=$op RL_PW=$opw
	((RK == 13 || RK == 10)) && RK=-1
	return 0
}
# the editing loop; status 0 line read (RL_B), 1 end of input, 130 ^C
rl_edit() {   # prompt
	local pr tabs=0 pushed=0 tab p a b w x l n first ret=0
	rl_prompt_expand "$1"
	pr=$R
	if [[ $pr == *$'\n'* ]]; then RL_PFIRST=${pr%$'\n'*}$'\n' RL_PLINE=${pr##*$'\n'}; else RL_PFIRST= RL_PLINE=$pr; fi
	rl_pwidth "$RL_PLINE"
	RL_PW=$R
	RL_B= RL_P=0 RL_ROW=0 RL_SAVED= RL_HIDX=${#RL_HIST[@]}
	rl_full_prompt
	if [[ -n $RL_PEND ]]; then   # lines left over from a paste
		if [[ $RL_PEND == *$'\n'* ]]; then
			rl_insert "${RL_PEND%%$'\n'*}"
			RL_PEND=${RL_PEND#*$'\n'}
			rl_to_end
			printf '\r\n' >&2
			return 0
		fi
		rl_insert "$RL_PEND"
		RL_PEND=
	fi
	rl_refresh
	while :; do
		((pushed)) || rl_readkey
		pushed=0 tab=0
		case $RK in
		$K_EOF) [[ -z $RL_B ]] && return 1 ;;
		13 | 10) rl_to_end; printf '\r\n' >&2; return 0 ;;
		1 | $K_HOME) RL_P=0 ;;
		5 | $K_END) RL_P=${#RL_B} ;;
		2 | $K_LEFT) ((RL_P > 0)) && ((RL_P--)) ;;
		6 | $K_RIGHT) ((RL_P < ${#RL_B})) && ((RL_P++)) ;;
		$K_WLEFT | $((K_META + 98))) rl_word_left; RL_P=$R ;;    # M-b
		$K_WRIGHT | $((K_META + 102))) rl_word_right; RL_P=$R ;; # M-f
		127 | 8) ((RL_P > 0)) && { RL_B=${RL_B:0:RL_P-1}${RL_B:RL_P}; ((RL_P--)); } ;;
		4 | $K_DEL)
			[[ $RK == 4 && -z $RL_B ]] && return 1
			((RL_P < ${#RL_B})) && RL_B=${RL_B:0:RL_P}${RL_B:RL_P+1}
			;;
		11) rl_kill $RL_P ${#RL_B} ;;   # ^K
		21) rl_kill 0 $RL_P ;;          # ^U
		23)                              # ^W: back to the previous blank
			p=$RL_P
			while ((p > 0)) && [[ ${RL_B:p-1:1} == [' '$'\t'] ]]; do ((p--)); done
			while ((p > 0)) && [[ ${RL_B:p-1:1} != [' '$'\t'] ]]; do ((p--)); done
			rl_kill $p $RL_P
			;;
		$((K_META + 127)) | $((K_META + 8))) rl_word_left; rl_kill $R $RL_P ;;
		$((K_META + 100))) rl_word_right; rl_kill $RL_P $R ;;   # M-d
		25) [[ -n $RL_KILL ]] && rl_insert "$RL_KILL" ;;      # ^Y
		20)                                                    # ^T
			if ((RL_P > 0 && ${#RL_B} > 1)); then
				((RL_P == ${#RL_B})) && ((RL_P--))
				a=${RL_B:RL_P-1:1} b=${RL_B:RL_P:1}
				RL_B=${RL_B:0:RL_P-1}$b$a${RL_B:RL_P+1}
				((RL_P++))
			fi
			;;
		$((K_META + 117)) | $((K_META + 108)) | $((K_META + 99)))   # M-u M-l M-c
			p=$RL_P n=${#RL_B}
			while ((p < n)) && ! rl_isword "${RL_B:p:1}"; do ((p++)); done
			a=$p
			while ((p < n)) && rl_isword "${RL_B:p:1}"; do ((p++)); done
			w=${RL_B:a:p-a}
			case $((RK - K_META)) in
			117) w=${w^^} ;;
			108) w=${w,,} ;;
			*) x=${w:1}; w=${w:0:1}; w=${w^}${x,,} ;;
			esac
			RL_B=${RL_B:0:a}$w${RL_B:p}
			RL_P=$p
			;;
		$((K_META + 46)) | $((K_META + 95)))   # M-. M-_: the last word of the previous command
			if ((${#RL_HIST[@]})); then
				rl_hwords "${RL_HIST[${#RL_HIST[@]} - 1]}"
				((${#RL_W[@]})) && rl_insert "${RL_W[${#RL_W[@]} - 1]}"
			fi
			;;
		$((K_META + 60)) | $K_PGUP) rl_hist_go 0 ;;
		$((K_META + 62)) | $K_PGDN) rl_hist_go ${#RL_HIST[@]} ;;
		12) printf '%s' "$RL_E[H$RL_E[2J" >&2; rl_full_prompt ;;   # ^L
		3) rl_to_end; printf '^C\r\n' >&2; return 130 ;;
		16 | $K_UP) rl_hist_go $((RL_HIDX - 1)) ;;
		14 | $K_DOWN) rl_hist_go $((RL_HIDX + 1)) ;;
		18)
			rl_rsearch
			if ((RK == -1)); then rl_to_end; printf '\r\n' >&2; return 0; fi
			((RK != K_NONE)) && { pushed=1; continue; }
			;;
		22) rl_readkey; [[ -n $RL_KEY ]] && rl_insert "$RL_KEY" ;;   # ^V: the next key as it is
		9) tab=1; rl_complete $tabs ;;
		$K_PASTE)   # bracketed paste: the text up to ESC[201~, typed in literally
			x=
			while __bashful_term key; do
				x+=$KEY
				[[ $x == *"$RL_E[201~" ]] && { x=${x%"$RL_E[201~"}; break; }
			done
			x=${x//$'\r'/$'\n'}
			if [[ $x != *$'\n'* ]]; then rl_insert "$x"
			else
				rl_insert "${x%%$'\n'*}"
				RL_PEND+=${x#*$'\n'}
				rl_to_end
				printf '\r\n' >&2
				return 0
			fi
			;;
		*)
			if ((RK >= 32 && RK < 256)) && [[ -n $RL_KEY ]]; then
				first=0
				((RL_P == ${#RL_B})) && first=1
				rl_insert "$RL_KEY"
				if ((first)) && [[ $RL_KEY != $RL_CTL ]]; then   # typing at the end: just echo it
					rl_cols
					__bashful_term width "$RL_B"
					w=$((RL_PW + REPLY))
					if ((w % RL_COLS && w / RL_COLS == RL_ROW)); then printf '%s' "$RL_KEY" >&2; tabs=0; continue; fi
				fi
			fi
			;;
		esac
		if ((tab)); then ((tabs++)); else tabs=0; fi
		rl_refresh
	done
}
rl_readline() {   # 1|2 -> REPLY; status 0, 1 at end of input, 130 for ^C
	local st prompt loc=
	rl_hist_load
	rl_hist_settings
	if [[ $1 == 2 ]]; then rl_var PS2 || R='> '; else rl_var PS1 || R='bashful:\w\$ '; fi
	prompt=$R
	if ! __bashful_term raw; then   # no terminal: a plain read
		rl_prompt_expand "$prompt"
		printf '%s' "${R//$RL_MARKS/}" >&2
		IFS= read -r REPLY
		st=$?
		((st)) && [[ -n $REPLY ]] && st=0
		return $st
	fi
	# the compiler works on bytes (LC_ALL=C, util.sh); editing is in the session's characters
	for st in LC_ALL LC_CTYPE LANG; do rl_var $st && [[ -n $R ]] && { loc=$R; break; }; done
	LC_ALL=${loc:-C}
	printf '%s' "$RL_E[?2004h" >&2
	rl_edit "$prompt"
	st=$?
	printf '%s' "$RL_E[?2004l" >&2
	__bashful_term restore
	LC_ALL=C
	REPLY=$RL_B
	return $st
}
