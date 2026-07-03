local function fibonacci( n )
    if n <= 0 then
        return 0
    elseif n == 1 then
        return 1
    else
        return fibonacci( n - 1 ) + fibonacci( n - 2 )
    end
end

target = 35
result = fibonacci( target )
print( "Fibonacci of " .. target .. " is: " .. result )
