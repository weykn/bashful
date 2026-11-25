# pushd, popd and dirs: bash's directory stack, written in bash and compiled into the scripts that use them
# (see shlib_use in check.sh).  The stack below the current directory is __bashful_dirs (element 0 is
# bash's DIRSTACK[1]); the current directory is always $PWD, so a plain cd changes the top, like in bash.
__bashful_dirs=()

__bashful_dirs_err() {   # builtin message [usage]
	__bashful_warn "$1: $2"
	if [[ -n $3 ]]; then printf '%s\n' "$1: usage: $3" >&2; fi
}
__bashful_dirs_tilde() {   # dir -> REPLY, with $HOME shown as ~ (bash's polite_directory_format)
	REPLY=$1
	if ((${#HOME} > 1)) && [[ $1 == "$HOME" || $1 == "$HOME"/* ]]; then REPLY="~${1:${#HOME}}"; fi
}
# +N / -N -> REPLY = the index in the whole stack ($PWD first); status 1 when it is not a number, 2 when it
# is out of range
__bashful_dirs_index() {   # arg count
	local n=${1:1}
	[[ $1 == [+-]* && -n $n && $n != *[!0-9]* ]] || return 1
	n=$((10#$n))
	if [[ $1 == -* ]]; then n=$(($2 - 1 - n)); fi
	((n >= 0 && n < $2)) || return 2
	REPLY=$n
}
__bashful_dirs_print() {   # long lines verbose [index]: what dirs prints
	local -a st
	local i d sep=
	st=("$PWD" "${__bashful_dirs[@]}")
	for ((i = 0; i < ${#st[@]}; i++)); do
		[[ -n $4 ]] && ((i != $4)) && continue
		d=${st[i]}
		if (($1)); then REPLY=$d; else __bashful_dirs_tilde "$d"; fi
		if (($3)); then printf '%2d  %s\n' $i "$REPLY"
		elif (($2)); then printf '%s\n' "$REPLY"
		else printf '%s%s' "$sep" "$REPLY"; sep=' '; fi
	done
	if ((!$2 && !$3)); then printf '\n'; fi
	return 0
}

__bashful_b_dirs() {
	local long=0 lines=0 verbose=0 clear=0 idx= a r
	local usage='dirs [-clpv] [+N] [-N]'
	for a; do
		case $a in
		-c) clear=1 ;;
		-l) long=1 ;;
		-p) lines=1 ;;
		-v) verbose=1 ;;
		[+-]*)
			__bashful_dirs_index "$a" $((${#__bashful_dirs[@]} + 1))
			r=$?
			if ((r == 1)); then __bashful_dirs_err dirs "$a: invalid number" "$usage"; return 2; fi
			if ((r == 2)); then
				if ((${#__bashful_dirs[@]})); then __bashful_dirs_err dirs "${a:1}: directory stack index out of range"
				else __bashful_dirs_err dirs "directory stack empty"; fi
				return 1
			fi
			idx=$REPLY
			;;
		*) __bashful_dirs_err dirs "$a: invalid option" "$usage"; return 2 ;;
		esac
	done
	if ((clear)); then __bashful_dirs=(); return 0; fi
	__bashful_dirs_print $long $lines $verbose $idx
}

__bashful_b_pushd() {
	local nocd=0 dir= a r old
	local -a st
	local usage='pushd [-n] [+N | -N | dir]'
	for a; do
		if [[ $a == -n ]]; then nocd=1
		elif [[ -n $dir ]]; then __bashful_dirs_err pushd "too many arguments"; return 1
		else dir=$a; fi
	done
	if [[ -z $dir ]]; then   # swap the top two
		if ((${#__bashful_dirs[@]} == 0)); then __bashful_dirs_err pushd "no other directory"; return 1; fi
		dir=${__bashful_dirs[0]}
		if ((nocd)); then __bashful_dirs_print 0 0 0; return 0; fi
		old=$PWD
		if ! cd -- "$dir" 2> /dev/null; then __bashful_dirs_err pushd "$dir: No such file or directory"; return 1; fi
		__bashful_dirs[0]=$old
	elif [[ $dir == [+-]* ]]; then   # rotate the stack so that entry N is on top
		__bashful_dirs_index "$dir" $((${#__bashful_dirs[@]} + 1))
		r=$?
		if ((r == 1)); then __bashful_dirs_err pushd "$dir: invalid number" "$usage"; return 2; fi
		if ((r == 2)); then
			if ((${#__bashful_dirs[@]})); then __bashful_dirs_err pushd "$dir: directory stack index out of range"
			else __bashful_dirs_err pushd "directory stack empty"; fi
			return 1
		fi
		st=("$PWD" "${__bashful_dirs[@]}")
		st=("${st[@]:REPLY}" "${st[@]:0:REPLY}")
		if ((!nocd)) && ! cd -- "${st[0]}" 2> /dev/null; then __bashful_dirs_err pushd "${st[0]}: No such file or directory"; return 1; fi
		__bashful_dirs=("${st[@]:1}")
	else
		if ((nocd)); then __bashful_dirs=("$dir" "${__bashful_dirs[@]}")
		else
			old=$PWD
			if ! cd -- "$dir" 2> /dev/null; then __bashful_dirs_err pushd "$dir: No such file or directory"; return 1; fi
			__bashful_dirs=("$old" "${__bashful_dirs[@]}")
		fi
	fi
	__bashful_dirs_print 0 0 0
}

__bashful_b_popd() {
	local nocd=0 n= a r i
	local usage='popd [-n] [+N | -N]'
	for a; do
		if [[ $a == -n ]]; then nocd=1
		elif [[ $a == [+-]* ]]; then n=$a
		else __bashful_dirs_err popd "$a: invalid argument" "$usage"; return 2; fi
	done
	if ((${#__bashful_dirs[@]} == 0)); then __bashful_dirs_err popd "directory stack empty"; return 1; fi
	i=0
	if [[ -n $n ]]; then
		__bashful_dirs_index "$n" $((${#__bashful_dirs[@]} + 1))
		r=$?
		if ((r == 1)); then __bashful_dirs_err popd "$n: invalid number" "$usage"; return 2; fi
		if ((r == 2)); then __bashful_dirs_err popd "$n: directory stack index out of range"; return 1; fi
		i=$REPLY
	fi
	if ((i == 0)); then   # the current directory goes: cd to the next one
		if ((!nocd)) && ! cd -- "${__bashful_dirs[0]}" 2> /dev/null; then
			__bashful_dirs_err popd "${__bashful_dirs[0]}: No such file or directory"
			return 1
		fi
		__bashful_dirs=("${__bashful_dirs[@]:1}")
	else
		__bashful_dirs=("${__bashful_dirs[@]:0:i-1}" "${__bashful_dirs[@]:i}")
	fi
	__bashful_dirs_print 0 0 0
}
