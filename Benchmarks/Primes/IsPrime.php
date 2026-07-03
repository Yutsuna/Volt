<?php
function is_prime(int $n): bool
{
    if ($n < 2) {
        return false;
    }

    $i = 2;
    while ($i * $i <= $n) {
        if ($n % $i == 0) {
            return false;
        }
        $i++;
    }
    return true;
}

function count_primes(int $limit): int
{
    $count = 0;
    for ($i = 2; $i < $limit; $i++) {
        if (is_prime($i)) {
            $count++;
        }
    }
    return $count;
}

$limit = 100_000;
$count = count_primes($limit);

echo "Number of primes up to $limit: $count\n";
?>
