# the time keyword: the report goes to stderr, the timed pipeline keeps its status and output
TIMEFORMAT='took %0R s (%0lR)'
{ time echo hello; } 2>&1
{ time false; } 2> /dev/null
echo "status $?"
{ time -p true; } 2>&1 | sed 's/[0-9]/N/g'
{ ! time false; } 2> /dev/null
echo "negated $?"
{ time echo a | tr a b; } 2>&1
i=0
{ time while ((i < 1000)); do ((i++)); done; } 2>&1
echo "i=$i"
TIMEFORMAT=
time echo quiet
x=$( { time echo captured; } 2>&1 )
echo "x=$x"
unset TIMEFORMAT
{ time sleep 0; } 2>&1 | sed 's/[0-9]/N/g'
