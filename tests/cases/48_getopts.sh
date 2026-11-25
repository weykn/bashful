# getopts (written in bash: shlib/getopts.sh): grouped options, arguments glued or separate, --, silent mode,
# OPTIND resets, getopts inside functions with local OPTIND, explicit argument lists
parse() {
	local OPTIND=1 o
	echo "== $*"
	while getopts "$spec" o "$@"; do echo "o=$o OPTARG=${OPTARG-unset} OPTIND=$OPTIND"; done
	echo "end o=$o OPTARG=${OPTARG-unset} OPTIND=$OPTIND"
	shift $((OPTIND - 1))
	echo "rest: $*"
}
spec="ab:c"
parse -a -b x -c rest
parse -abx -ac more
parse -b
parse -- -a
parse - -a
parse -a - -b
parse -acb
parse -z -a 2> /dev/null
spec=":ab:"
parse -z -b
parse -bvalue -a
OPTERR=0 spec="ab:"
parse -z -b
OPTERR=1
echo "OPTIND=$OPTIND OPTERR=$OPTERR"
set -- -x -y arg
while getopts xy o; do echo "top $o $OPTIND"; done
shift $((OPTIND - 1)); echo "left: $1"
OPTIND=1; set -- -ab -a
getopts ab o; echo "$o $OPTIND"
OPTIND=1
getopts ab o; echo "after reset: $o $OPTIND"
getopts ab o; echo "$o $OPTIND"
getopts ab o; echo "$o $OPTIND"
getopts ab o; echo "status $? $o $OPTIND"
getopts 2> /dev/null; echo "usage status $?"
getopts ab 1x 2> /dev/null; echo "bad name status $?"
