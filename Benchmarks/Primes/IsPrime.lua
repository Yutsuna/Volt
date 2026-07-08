local function is_prime(n)
    if n < 2 then
        return false
    end

    local i = 2
    while i * i <= n do
        if n % i == 0 then
            return false
        end
        i = i + 1
    end
    return true
end

local function count_primes(limit)
    local count = 0
    for i = 2, limit do
        if is_prime(i) then
            count = count + 1
        end
    end
    return count
end

local limit = 100000
local count = count_primes(limit)
print("Number of primes up to " .. limit .. ": " .. count)
