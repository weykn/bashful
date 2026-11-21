# stdin-driven text processing; run against every 27_stdin_text.*.in (edge-case inputs).
# Everything printed depends on the input, so no answer can be known at compile time.
show() { printf '<%s>' "$@"; echo; }
nl=0 words=0 chars=0 blank=0 longest="" maxlen=0
declare -A freq
lines=()
while IFS= read -r line || [[ -n $line ]]; do
	((nl++))
	lines+=("$line")
	((chars += ${#line}))
	[[ -z ${line//[[:space:]]/} ]] && ((blank++))
	if ((${#line} > maxlen)); then maxlen=${#line}; longest=$line; fi
	read -ra ws <<< "$line"
	((words += ${#ws[@]}))
	for w in "${ws[@]}"; do
		w=${w,,}; w=${w//[^a-z0-9]/}
		[[ -n $w ]] && freq[$w]=$((${freq[$w]:-0} + 1))
	done
done
echo "lines=$nl words=$words chars=$chars blank=$blank maxlen=$maxlen"
show "$longest"

# word frequencies, sorted deterministically (count desc, word asc)
for w in "${!freq[@]}"; do echo "${freq[$w]} $w"; done | sort -k1,1nr -k2,2 | head -5

# per-line transformations
i=0
for line in "${lines[@]}"; do
	((i++ < 40)) || break
	t=${line#"${line%%[![:space:]]*}"}; t=${t%"${t##*[![:space:]]}"}
	case $line in
	'') kind=empty ;;
	*[[:space:]]) kind=trailing-ws ;;
	[0-9]*) kind=numeric-start ;;
	-*) kind=dash ;;
	\#*) kind=comment ;;
	*'*'* | *'?'* | *'['*) kind=globby ;;
	*) kind=text ;;
	esac
	printf '%3d %-13s |%s| up=%s rev1=%s len=%d\n' "$i" "$kind" "$t" "${t^^}" "${t: -1}" "${#t}"
	[[ $line =~ ([0-9]+)[^0-9]+([0-9]+) ]] && echo "    nums: ${BASH_REMATCH[1]} ${BASH_REMATCH[2]}"
	[[ $line == *$'\r' ]] && echo "    has CR"
	IFS=, read -r f1 f2 rest <<< "$line"; show "$f1" "$f2" "$rest"
	set -- $line; echo "    argc=$# first=${1-unset}"
done

# echo/printf with data that looks like options and format strings
for line in "${lines[@]:0:15}"; do
	echo "$line"
	printf '%s\n' "$line"
	printf -- "$line" 2>/dev/null; echo " [rc=$?]"
done

# unquoted expansion: word splitting + globbing against a known directory
mkdir -p g; touch g/a1 g/b2 g/c3
cd g
for line in "${lines[@]:0:20}"; do echo "glob:" $line; done
cd ..

# read without -r: backslash handling and line continuation
printf '%s\n' "${lines[@]}" | while read a b; do show "$a" "$b"; done | head -30
echo "rc=$nl"
exit $((nl % 7))
