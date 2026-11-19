# features outside the native subset: compiled with the embedded-bash fallback, must behave exactly like bash
declare -A m=([one]=1 [two]=2)
for k in $(printf '%s\n' "${!m[@]}" | sort); do echo "$k=${m[$k]}"; done
set -- -a x -b
while getopts "a:b" o; do echo "opt $o ${OPTARG-}"; done
echo "line $LINENO"
shopt -s nullglob; f=(/nonexistent/*); echo "${#f[@]}"
x=hello; echo "${x@Q}"
