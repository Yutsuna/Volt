def is_prime( n )
  return false if n < 2

  i = 2
  while i * i <= n
    return false if n % i == 0
    i += 1
  end
  true
end

def count_primes( limit )
  count = 0
  ( 2...limit ).each { |n| count += 1 if is_prime n }
  count
end

limit = 100_000
count = count_primes limit
puts "Number of primes up to #{limit}: #{count}"
