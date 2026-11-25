# repl.sh - interactive mode (bashful -i): a shell whose every command is compiled and run natively
#
# Each command is its own program, so the shell's state travels between them as code: on exit the runtime
# writes the variables, exports, working directory, positional parameters and set -e/-u/pipefail to a state
# file (BASHFUL_REPL, see repl_dump in rt_frames.c), and that text goes in front of the next command.  Functions
# are kept as their source text, cut out of the input by the parser.  With BASHFUL_REPL_NOENV the program does
# not import the environment (the prelude has it), so `unset`ting an exported variable sticks.  A command the
# compiler cannot translate runs on the bash fallback, wrapped in a bash version of the same state dump.
#
# Like bash -i: /etc/bash.bashrc, ~/.bashrc (and ~/.bashfulrc) run first, $- has i, bash's defaults for PS1,
# HISTFILE, HISTSIZE, ... are set, aliases are expanded and PROMPT_COMMAND runs after every command.  The
# commands that change aliases (alias, unalias, source) run on the fallback, whose dump also writes them
# ($state.alias) and the functions the command defined without the REPL seeing their text, e.g. in a sourced
# file ($state.fn).  Both dumps write PROMPT_COMMAND's commands to $state.pc.

REPL_BASH_PRE='__bashful_state=$BASHFUL_REPL
unset BASHFUL_REPL BASHFUL_REPL_NOENV BASHFUL_REPL_I
__bashful_dump() {
	local __bashful_s=$? __bashful_o=$- __bashful_n __bashful_k __bashful_a __bashful_pf=0
	[[ -o pipefail ]] && __bashful_pf=1
	set +eu
	{
	printf "%s\n" "#bashful-state"
	for __bashful_n in $(compgen -v); do
		case $__bashful_n in
		__bashful_* | BASH* | HISTCMD | PPID | UID | EUID | SHELLOPTS | COMP_* | FUNCNAME | GROUPS | DIRSTACK | \
			PIPESTATUS | LINENO | RANDOM | SECONDS | SRANDOM | EPOCHSECONDS | EPOCHREALTIME | OPTIND | OPTERR | OPTARG | \
			PWD | _ | COLUMNS | LINES) continue ;;
		esac
		__bashful_a=${!__bashful_n@a}
		[[ $__bashful_a == *r* ]] && continue
		declare -n __bashful_r=$__bashful_n
		if [[ $__bashful_a == *A* ]]; then
			printf "declare -A %s\n%s=()\n" "$__bashful_n" "$__bashful_n"
			for __bashful_k in "${!__bashful_r[@]}"; do printf "%s[%s]=%s\n" "$__bashful_n" "${__bashful_k@Q}" "${__bashful_r[$__bashful_k]@Q}"; done
		elif [[ $__bashful_a == *a* ]]; then
			printf "%s=()\n" "$__bashful_n"
			for __bashful_k in "${!__bashful_r[@]}"; do printf "%s[%s]=%s\n" "$__bashful_n" "$__bashful_k" "${__bashful_r[$__bashful_k]@Q}"; done
		elif [[ -n ${__bashful_r+x} ]]; then
			printf "%s=%s\n" "$__bashful_n" "${__bashful_r@Q}"
			[[ $__bashful_a == *x* ]] && printf "export %s\n" "$__bashful_n"
		fi
		unset -n __bashful_r
	done
	printf "cd %s 2> /dev/null\nset --" "${PWD@Q}"
	for __bashful_k; do printf " %s" "${__bashful_k@Q}"; done
	printf "\n#bashful-opts\n"
	[[ $__bashful_o == *u* ]] && printf "set -u\n"
	((__bashful_pf)) && printf "set -o pipefail\n"
	[[ $__bashful_o == *e* ]] && printf "set -e\n"
	printf "#bashful-pwd %s\n" "$PWD"
	if [[ -n ${__bashful_end+x} ]]; then printf "#bashful-end %s\n" "$__bashful_end"; else printf "#bashful-exit %s\n" "$__bashful_s"; fi
	} > "$__bashful_state"
	if [[ -n ${PROMPT_COMMAND+x} ]]; then printf "%s\n" "${PROMPT_COMMAND[@]}" > "$__bashful_state.pc"; fi
	alias -p > "$__bashful_state.alias"
	for __bashful_n in $(compgen -A function); do
		case $__bashful_n in __bashful_*) continue ;; esac
		case $__bashful_known in *" $__bashful_n "*) continue ;; esac
		printf "%s\1%s\n\1" "$__bashful_n" "$(declare -f "$__bashful_n")"
	done > "$__bashful_state.fn"
}
trap '"'"'__bashful_dump "$@"'"'"' EXIT
'

repl_sq() {   # text -> R = text single-quoted for bash
	local q=\'
	R=$q${1//$q/$q\\$q$q}$q
}
# Parse the input so far (run in a subshell: the parser exits on errors).  Status 0: complete, 4: more lines
# needed, 5: syntax error (the command still runs, so that the error message is the real one).  Prints the
# top-level function definitions as name \1 source text \1 ...
repl_probe() {   # text
	local l c start
	PROBE=1 S=$1 I=0 PEND= LINEBASE=0 NOREBASE=1 ARMED=0
	while :; do
		skip_ws 1
		while [[ ${S:I:1} == ';' && ${S:I+1:1} != ';' ]]; do ((I++)); skip_ws 1; done
		[[ -z ${S:I:1} ]] && break
		at_closer && exit 5
		start=$I
		parse_andor
		l=$R
		((NT[l] == N_FUNC)) && printf '%s\1%s\1' "${NNAME[l]}" "${S:start:I-start}"
		skip_ws 0
		c=${S:I:1}
		if [[ $c == '&' && ${S:I+1:1} != '&' ]]; then ((I++))
		elif [[ $c == ';' && ${S:I+1:1} != ';' ]]; then ((I++))
		elif [[ $c == $'\n' || -z $c ]]; then :
		else exit 5
		fi
	done
	exit 0
}
# `alias -p` output -> ALIAS[_name]=value, ALIASDEF = the same text (put in front of fallback commands, so that
# `alias` lists them and `unalias` works)
repl_aliases_load() {   # text
	local l n v q=\'
	ALIAS=() ALIASDEF=$1
	while IFS= read -r l; do
		[[ $l == 'alias '*=* ]] || continue
		l=${l#alias }
		[[ $l == '-- '* ]] && l=${l#-- }
		n=${l%%=*}
		v=${l#*=}
		if [[ $v == "$q"*"$q" ]]; then v=${v:1:${#v}-2}; v=${v//"$q\\$q$q"/$q}; fi
		ALIAS[_$n]=$v
	done <<< "$1"
}
# the end of a word starting at I in S (quotes, backslashes, $(...), ${...} and `...` are part of it) -> I
repl_word_end() {
	local c d
	while ((I < N)); do
		c=${S:I:1}
		case $c in
		' ' | $'\t' | $'\n' | ';' | '&' | '|' | '(' | ')' | '<' | '>') return 0 ;;
		\\) ((I += 2)) ;;
		\') ((I++)); while ((I < N)) && [[ ${S:I:1} != \' ]]; do ((I++)); done; ((I++)) ;;
		\") ((I++)); while ((I < N)) && [[ ${S:I:1} != \" ]]; do [[ ${S:I:1} == \\ ]] && ((I++)); ((I++)); done; ((I++)) ;;
		\`) ((I++)); while ((I < N)) && [[ ${S:I:1} != \` ]]; do [[ ${S:I:1} == \\ ]] && ((I++)); ((I++)); done; ((I++)) ;;
		\$)
			c=${S:I+1:1}
			if [[ $c == '(' || $c == '{' ]]; then   # balanced, quotes inside skipped roughly
				((I += 2)); d=1
				while ((I < N && d > 0)); do
					case ${S:I:1} in
					'(' | '{') ((d++)) ;;
					')' | '}') ((d--)) ;;
					\\) ((I++)) ;;
					\') ((I++)); while ((I < N)) && [[ ${S:I:1} != \' ]]; do ((I++)); done ;;
					esac
					((I++))
				done
			else ((I++)); fi
			;;
		*) ((I++)) ;;
		esac
	done
}
# bash's alias expansion on the command words of TEXT (the first word of each simple command, and the word after
# an alias whose value ends in a blank) -> R.  Aliases inside $(...) and `...` are not expanded.
repl_alias_expand() {   # text names-being-expanded
	local S=$1 N=${#1} I=0 out= c w st cmd=1 incase=0 hd= d strip ln e rest v seen=$2 sub
	while ((I < N)); do
		c=${S:I:1}
		case $c in
		' ' | $'\t') out+=$c; ((I++)) ;;
		$'\n')
			out+=$c; ((I++)); cmd=1
			for d in $hd; do   # here-document bodies are copied as they are
				strip=0
				[[ $d == -* ]] && strip=1
				d=${d#?}
				while ((I < N)); do
					e=$I
					while ((e < N)) && [[ ${S:e:1} != $'\n' ]]; do ((e++)); done
					ln=${S:I:e-I}
					out+=${S:I:e+1-I}
					I=$((e + 1))
					((strip)) && while [[ $ln == $'\t'* ]]; do ln=${ln:1}; done
					[[ $ln == "$d" ]] && break
				done
			done
			hd=
			;;
		';' | '&' | '|' | '(')
			if [[ $c == ';' && ${S:I+1:1} == ';' ]]; then out+=';;'; ((I += 2)); cmd=0; continue; fi
			out+=$c; ((I++)); cmd=1
			;;
		')') out+=$c; ((I++)); cmd=$((incase > 0)) ;;   # in a case: the end of a pattern
		'<' | '>')
			st=$I
			while [[ ${S:I:1} == [\<\>\&\|-] ]]; do ((I++)); done
			w=${S:st:I-st}
			out+=$w
			if [[ $w == '<<' || $w == '<<-' ]]; then   # remember the here-document's delimiter
				while [[ ${S:I:1} == [' '$'\t'] ]]; do out+=${S:I:1}; ((I++)); done
				st=$I
				repl_word_end
				w=${S:st:I-st}
				out+=$w
				w=${w//\'/}; w=${w//\"/}; w=${w//\\/}
				if [[ ${S:st-1:1} == - || ${S:st-2:2} == '- ' ]]; then hd+=" -$w"; else hd+=" +$w"; fi
			fi
			;;
		'#')
			if ((I == 0)) || [[ ${S:I-1:1} == [' '$'\t'$'\n'';&|()'] ]]; then
				while ((I < N)) && [[ ${S:I:1} != $'\n' ]]; do out+=${S:I:1}; ((I++)); done
			else st=$I; repl_word_end; out+=${S:st:I-st}; cmd=0; fi
			;;
		*)
			st=$I
			repl_word_end
			((I == st)) && ((I++))
			w=${S:st:I-st}
			if ((!cmd)); then out+=$w; continue; fi
			case $w in
			if | then | else | elif | do | while | until | '!' | '{' | time) out+=$w; continue ;;
			'case') out+=$w; ((incase++)); cmd=0; continue ;;
			'esac') out+=$w; ((incase > 0 && incase--)); cmd=0; continue ;;
			[a-zA-Z_]*=*)   # an assignment: the next word is still the command
				v=${w%%=*}
				v=${v%+}
				v=${v%%\[*}
				[[ $v == *[!a-zA-Z0-9_]* ]] || { out+=$w; continue; }
				;;
			esac
			cmd=0
			if [[ -n ${ALIAS[_$w]+x} && $seen != *" $w "* && $w != *[\\\'\"\$\`]* ]]; then
				if ((incase)); then   # a case pattern, not a command
					rest=${S:I}
					rest=${rest#"${rest%%[![:space:]]*}"}
					[[ $rest == ')'* || $rest == '|'* ]] && { out+=$w; continue; }
				fi
				v=${ALIAS[_$w]}
				repl_alias_expand "$v" "$seen $w "
				out+=$R
				[[ $v == *[' '$'\t'] ]] && cmd=1
			else out+=$w; fi
			;;
		esac
	done
	R=$out
}
# the startup files of an interactive bash (Arch and Debian build bash with /etc/bash.bashrc), then ~/.bashfulrc
# -> R.  `$-` becomes ${__bashful_dash-$-}, so that the usual `[[ $- != *i* ]] && return` passes on the bash
# fallback as well; bash-completion is not loaded (its completions do not exist here, and its hundreds of
# functions would slow down every command).
repl_rcfiles() {
	local f t l out=
	for f in /etc/bash.bashrc ${HOME:+"$HOME/.bashrc" "$HOME/.bashfulrc"}; do
		[[ -r $f && ! -d $f ]] || continue
		t=
		IFS= read -r -d '' t < "$f"
		while IFS= read -r l; do
			case $l in *bash_completion*)
				case ${l#"${l%%[![:space:]]*}"} in '. '* | 'source '*) l=: ;; esac
				;;
			esac
			l=${l//'${-}'/'${__bashful_dash-$-}'}
			l=${l//'$-'/'${__bashful_dash-$-}'}
			out+=$l$'\n'
		done <<< "$t"
	done
	R=$out
}
# On a terminal, lines are read with the line editor (readline.sh: keys, history, completion,
# PS1/PS2), which gets the session's state and function names to complete from.  Interactive (a terminal, or
# -i), the startup files run first.
repl() {   # positional parameters for the session
	local line chunk t st rc=0 eof tty=0 cont fromrc pwd bin sf out name text a fns opts state errexit rcfile= inter=0
	local pc= oldpc v x known pcrun=0
	local -A FNTEXT ALIAS
	local -a FNORDER
	ALIASDEF=
	bin=/tmp/bashful.$$.repl sf=/tmp/bashful.$$.state
	[[ -t 0 ]] && tty=1
	((tty || REPL_RC)) && inter=1
	state="set --"
	for a in "$@"; do repl_sq "$a"; state+=" $R"; done
	opts= pwd=$PWD
	export BASHFUL_REPL=$sf
	trap ':' INT   # ^C stops the running command, not the shell (a handler, not '': children get SIGINT back)
	BF_EPILOGUE=
	QUIET=1
	if ((inter)); then
		export BASHFUL_REPL_I=1
		# the variables bash -i sets
		x=$(bash -c 'printf "%s\1%s\1%s\1%s" "$HOSTNAME" "$HOSTTYPE" "$MACHTYPE" "$OSTYPE"' 2> /dev/null)
		t=
		for v in HOSTNAME HOSTTYPE MACHTYPE OSTYPE; do
			[[ -n $x ]] && { repl_sq "${x%%$'\1'*}"; t+="$v=$R"$'\n'; }
			[[ $x == *$'\1'* ]] && x=${x#*$'\1'} || x=
		done
		[[ -n ${PS1+x} ]] || t+="PS1='\\s-\\v\\\$ '"$'\n'
		[[ -n ${PS2+x} ]] || t+="PS2='> '"$'\n'
		[[ -n ${PS4+x} ]] || t+="PS4='+ '"$'\n'
		if [[ -z ${HISTFILE+x} && -n $HOME ]]; then repl_sq "$HOME/.bash_history"; t+="HISTFILE=$R"$'\n'; fi
		[[ -n ${HISTSIZE+x} ]] || t+="HISTSIZE=500"$'\n'
		[[ -n ${HISTFILESIZE+x} ]] || t+="HISTFILESIZE=500"$'\n'
		state=$t$state
		((REPL_NORC)) || { repl_rcfiles; rcfile=$R; }
	fi
	while :; do
		chunk= eof=0 cont=0 fromrc=0
		if ((pcrun == 1)); then   # PROMPT_COMMAND changed: its new value runs before the next prompt
			chunk="(exit $rc)"$'\n' fromrc=1 pcrun=2 out= x=$rc
			[[ $opts == *'set -e'* ]] && chunk=$':\n'   # no errexit for it
		elif [[ -n $rcfile ]]; then
			pcrun=0
			chunk=$rcfile$'\n' rcfile= fromrc=1
			out=$(repl_probe "$chunk")
		else
			pcrun=0
			RL_STATE=$state RL_FUNCS="${FNORDER[*]}"
			for a in "${!ALIAS[@]}"; do RL_FUNCS+=" ${a#_}"; done
			while :; do
				line=
				if ((tty)); then
					rl_readline $((cont ? 2 : 1))
					st=$?
					line=$REPLY
					((st == 130)) && { rc=130; continue 2; }   # ^C: the input so far is dropped
					if ((st)); then eof=1; break; fi
					rl_history expand "$line"
					case $? in
					1) line=$REPLY; printf '%s\n' "$line" ;;
					2) continue 2 ;;
					3) printf '%s\n' "$REPLY"; rl_history add "$REPLY"; continue 2 ;;
					esac
				elif ! IFS= read -r line; then
					eof=1
					[[ -z $line ]] && break
				fi
				chunk+=$line$'\n'
				cont=1
				[[ $line == *\\ && $line != *\\\\ ]] && continue   # trailing backslash: line continuation
				t=${chunk%"${chunk##*[![:space:]]}"}
				if [[ $t == *'|' || $t == *'&&' ]] && [[ $t != *'\|' && $t != *'\&&' ]]; then continue; fi
				out=$(repl_probe "$chunk")
				st=$?
				((st == 4 && !eof)) || break
			done
		fi
		if [[ -z $chunk ]]; then   # end of input
			((tty)) && printf 'exit\n' >&2
			rm -f "$bin" "$sf" "$sf.pc" "$sf.alias" "$sf.fn"
			exit $rc
		fi
		[[ $chunk == *[![:space:]]* ]] || continue
		((tty && !fromrc)) && rl_history add "${chunk%$'\n'}"
		t=${chunk#"${chunk%%[![:space:]]*}"}
		t=${t%"${t##*[![:space:]]}"}
		case $t in   # bash's history builtin, for the history this shell keeps
		history) rl_history list; rc=$?; continue ;;
		'history -c') rl_history clear; rc=$?; continue ;;
		history\ [0-9]*) rl_history list "${t#history }"; rc=$?; continue ;;
		esac
		if ((inter && ${#ALIAS[@]})); then
			repl_alias_expand "$chunk" " "
			if [[ $R != "$chunk" ]]; then
				chunk=$R
				out=$(repl_probe "$chunk")
			fi
		fi
		while [[ $out == *$'\1'*$'\1'* ]]; do   # remember the functions it defines
			name=${out%%$'\1'*}
			out=${out#*$'\1'}
			text=${out%%$'\1'*}
			out=${out#*$'\1'}
			[[ -n ${FNTEXT[_$name]+x} ]] || FNORDER+=("$name")
			FNTEXT[_$name]=$text
		done
		fns= known=' '
		for name in "${FNORDER[@]}"; do fns+=${FNTEXT[_$name]}$'\n' known+="$name "; done
		repl_sq "$known"
		BF_PROLOGUE=$REPL_BASH_PRE"__bashful_known=$R"$'\n'
		((inter)) && BF_PROLOGUE+='__bashful_dash=${-}i'$'\n'
		[[ -n $ALIASDEF ]] && BF_PROLOGUE+=$ALIASDEF$'\n'
		errexit=0
		[[ $opts == *'set -e'* ]] && errexit=1
		SRC=$state$'\n'$fns$opts
		((errexit)) || SRC+="(exit $rc)"$'\n'
		SRC+=$chunk$'__bashful_end=$?\n'
		# PROMPT_COMMAND, with $? the command's status (a command that may change it runs it as a separate step)
		if [[ -n $pc && $chunk != *PROMPT_COMMAND* ]] && ((inter)); then
			((errexit)) || SRC+=$'(exit $__bashful_end)\n'
			SRC+=$pc
		fi
		rm -f "$sf" "$sf.pc" "$sf.alias" "$sf.fn"
		if ! (build_exe bashful "$bin"); then rc=1; continue; fi
		(exec -a bashful "$bin")
		rc=$?
		((pcrun == 2)) && rc=$x   # the status stays the last command's
		oldpc=$pc
		if [[ -r $sf ]]; then pc=; [[ -r $sf.pc ]] && IFS= read -r -d '' pc < "$sf.pc"; fi
		[[ -n $pc ]] && [[ $pc != "$oldpc" || $chunk == *PROMPT_COMMAND* ]] && ((inter && !pcrun)) && pcrun=1
		if [[ -r $sf.alias ]]; then t=; IFS= read -r -d '' t < "$sf.alias"; repl_aliases_load "$t"; fi
		if [[ -s $sf.fn ]]; then   # functions defined out of sight (a sourced file, eval, ...)
			IFS= read -r -d '' out < "$sf.fn"
			while [[ $out == *$'\1'*$'\1'* ]]; do
				name=${out%%$'\1'*}
				out=${out#*$'\1'}
				text=${out%%$'\1'*}
				out=${out#*$'\1'}
				[[ -n ${FNTEXT[_$name]+x} ]] && continue
				FNORDER+=("$name")
				FNTEXT[_$name]=$text
			done
		fi
		if [[ -r $sf ]]; then
			IFS= read -r -d '' t < "$sf"
			export BASHFUL_REPL_NOENV=1   # from now on the state has the environment
			state=${t%$'\n#bashful-opts\n'*}
			t=${t##*$'\n#bashful-opts\n'}
			opts=${t%%'#bashful-pwd '*}
			pwd=${t#*'#bashful-pwd '}
			pwd=${pwd%%$'\n'*}
			cd "$pwd" 2> /dev/null   # follow the session: relative paths complete from there, \w shows it
			t=${t##*'#bashful-'}
			case $t in
			end\ *) t=${t#end }; rc=${t%%$'\n'*} ;;
			exit\ *) t=${t#exit }; rm -f "$bin" "$sf" "$sf.pc" "$sf.alias" "$sf.fn"; exit ${t%%$'\n'*} ;;
			esac
		elif ((tty && rc > 128)); then
			printf '\n'   # killed by a signal (^C): the state stays what it was
		fi
	done
}
