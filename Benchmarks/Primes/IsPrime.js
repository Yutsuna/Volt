function is_prime( n )
{
    if (n < 2) {
        return false;
    }

    for (var i = 2; i * i <= n; ++i) {
        if (n % i == 0) {
            return false;
        }
    }
    return true;
}

function count_primes( n )
{
    var count = 0;

    for (var i = 2; i <= n; ++i) {
        if (is_prime(i)) {
            ++count;
        }
    }
    return count;
}

limit = 100_000;
count = count_primes(limit)
console.log( "Number of primes up to " + limit + ": " + count );
