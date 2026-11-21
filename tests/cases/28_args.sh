# argument-driven behaviour; run once per line of 28_args.args.  Exercises $#, $@/$*, shift, getopts,
# arithmetic on user-supplied values, and exit status derived from the arguments.
show() { printf '<%s>' "$@"; echo; }
echo "argc=$#"
show "$@"
show "$*"
show $@
IFS=,; echo "star-ifs: $*"; unset IFS
(($#)) && last=${!#} || last=none
echo "first=${1-unset} second=${2:-default} last=$last"

verbose=0 out=- count=1
OPTIND=1
while getopts ":vo:n:" opt; do
	case $opt in
	v) ((verbose++)) ;;
	o) out=$OPTARG ;;
	n) count=$OPTARG ;;
	:) echo "missing arg for -$OPTARG" ;;
	\?) echo "bad option -$OPTARG" ;;
	esac
done
shift $((OPTIND - 1))
echo "verbose=$verbose out=$out count=$count rest=$#"

sum=0 nonnum=0
for a in "$@"; do
	if [[ $a =~ ^-?[0-9]+$ ]]; then ((sum += 10#${a#-} * (${a:0:1} == "-" ? -1 : 1))); else ((nonnum++)); fi
done
echo "sum=$sum nonnum=$nonnum"

if [[ $count =~ ^[0-9]+$ ]] && ((count <= 50)); then
	for ((i = 0; i < count; i++)); do printf '%d ' $((i * i)); done; echo
fi

i=0
while (($#)); do
	((i++))
	printf '%d: %q len=%d\n' $i "$1" ${#1}
	case $1 in
	*=*) k=${1%%=*} v=${1#*=}; echo "   kv: $k -> $v" ;;
	/*) echo "   path base=${1##*/} dir=${1%/*}" ;;
	esac
	shift
done
exit $((sum & 255))
