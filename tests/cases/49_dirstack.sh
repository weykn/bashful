# pushd / popd / dirs (written in bash: shlib/dirs.sh)
HOME=/tmp
cd /
pushd /tmp; pushd /usr; pushd /etc
dirs; dirs -v; dirs -l; dirs -p; dirs +1; dirs -1
pushd; pushd +2; pushd -1
popd; popd +1; popd -0; popd
popd 2> /dev/null; echo "empty: $?"
pushd 2> /dev/null; echo "no other: $?"
pushd /nonexistent_dir_xyz 2> /dev/null; echo "missing: $?"
pushd -n /var; dirs; popd -n; dirs
dirs +5 2> /dev/null; echo "range: $?"
pushd -x 2> /dev/null; echo "invalid: $?"
popd a 2> /dev/null; echo "argument: $?"
dirs -c; dirs
x=$(pushd /usr > /dev/null; pwd); echo "subshell: $x, still in $PWD"; dirs
f() { pushd /etc > /dev/null; }
f; echo "after f: $PWD"; dirs -v
