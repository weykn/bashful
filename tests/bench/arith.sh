# integer arithmetic: sieve of Eratosthenes, collatz lengths, gcd
n=60000
sieve=()
for ((i = 2; i * i <= n; i++)); do
	if [[ -z ${sieve[i]} ]]; then
		for ((j = i * i; j <= n; j += i)); do sieve[j]=1; done
	fi
done
count=0
for ((i = 2; i <= n; i++)); do [[ -z ${sieve[i]} ]] && ((count++)); done
echo "primes below $n: $count"

best=0 bestn=0
for ((k = 1; k < 6000; k++)); do
	x=$k len=1
	while ((x != 1)); do
		if ((x % 2)); then ((x = 3 * x + 1)); else ((x /= 2)); fi
		((len++))
	done
	((len > best)) && { best=$len; bestn=$k; }
done
echo "longest collatz below 6000: $bestn ($best steps)"

gcd() { local a=$1 b=$2 t; while ((b)); do t=$((a % b)); a=$b; b=$t; done; REPLY=$a; }
sum=0
for ((a = 1; a < 120; a++)); do for ((b = 1; b < 120; b++)); do gcd $a $b; ((sum += REPLY)); done; done
echo "gcd sum: $sum"
