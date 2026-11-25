# getopts optstring name [arg ...]: bash's getopts, written in bash and compiled into the scripts that use it
# (see shlib_use in check.sh).  Calls are renamed to __bashful_b_getopts and given "$@" when they name no
# arguments, because a function cannot see its caller's positional parameters.
#
# Bash keeps its position inside a group of options (-abc) in the shell and starts over whenever OPTIND is
# assigned.  Here that position is __bashful_optpos, trusted only while OPTIND has not been assigned since
# getopts last set it: __bashful_varver (a runtime builtin) tells, from the variable's version number.
__bashful_optpos=1 __bashful_optver=
__bashful_b_getopts() {
	local spec name ind arg c rest quiet=0 nargs
	if (($# < 2)); then
		printf '%s\n' "getopts: usage: getopts optstring name [arg ...]" >&2
		return 2
	fi
	spec=$1 name=$2
	shift 2
	if [[ $name != [a-zA-Z_]* || $name == *[!a-zA-Z0-9_]* ]]; then
		__bashful_warn "getopts: \`$name': not a valid identifier"
		return 1
	fi
	[[ $spec == :* ]] && quiet=1
	nargs=$#
	ind=${OPTIND:-1}
	[[ $ind == *[!0-9]* || -z $ind ]] && ind=1
	((ind < 1)) && ind=1
	__bashful_varver OPTIND __bashful_v
	[[ $__bashful_v == "$__bashful_optver" ]] || __bashful_optpos=1
	if ((__bashful_optpos == 1)); then   # at the start of an argument: is it an option at all?
		arg=
		((ind <= nargs)) && arg=${!ind}
		if ((ind > nargs)) || [[ $arg != -?* ]]; then
			__bashful_getopts_end "$name" $ind
			return 1
		fi
		if [[ $arg == -- ]]; then
			__bashful_getopts_end "$name" $((ind + 1))
			return 1
		fi
	else
		arg=${!ind}
	fi
	c=${arg:__bashful_optpos:1}
	((__bashful_optpos++))
	if ((__bashful_optpos >= ${#arg})); then   # the last letter of this argument
		((ind++))
		__bashful_optpos=1
	fi
	rest=${spec#*"$c"}
	if [[ $c == : || $rest == "$spec" ]]; then   # not in optstring
		if ((quiet)); then OPTARG=$c
		else
			unset OPTARG
			[[ $OPTERR == 0 ]] || printf '%s: illegal option -- %s\n' "$0" "$c" >&2   # (bash: no line here)
		fi
		printf -v "$name" '%s' '?'
	elif [[ $rest == :* ]]; then   # takes an argument: the rest of this one, or the next
		if ((__bashful_optpos > 1)); then
			OPTARG=${arg:__bashful_optpos}
			((ind++))
			__bashful_optpos=1
			printf -v "$name" '%s' "$c"
		elif ((ind <= nargs)); then
			OPTARG=${!ind}
			((ind++))
			printf -v "$name" '%s' "$c"
		elif ((quiet)); then
			OPTARG=$c
			printf -v "$name" '%s' ':'
		else
			unset OPTARG
			[[ $OPTERR == 0 ]] || printf '%s: option requires an argument -- %s\n' "$0" "$c" >&2
			printf -v "$name" '%s' '?'
		fi
	else
		unset OPTARG
		printf -v "$name" '%s' "$c"
	fi
	OPTIND=$ind
	__bashful_varver OPTIND __bashful_optver
	return 0
}
__bashful_getopts_end() {   # name optind: the end of the options
	unset OPTARG
	printf -v "$1" '%s' '?'
	OPTIND=$2 __bashful_optpos=1
	__bashful_varver OPTIND __bashful_optver
}
