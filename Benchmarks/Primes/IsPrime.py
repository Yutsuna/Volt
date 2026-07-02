def is_prime( n : int ) -> bool:
    if n < 2:
        return False

    i = 2
    while i * i <= n:
        if n % i == 0:
            return False
        i += 1
    return True

def count_primes( limit : int ) -> int:
    count = 0
    for i in range( 2, limit + 1 ):
        if is_prime( i ):
            count += 1
    return count
