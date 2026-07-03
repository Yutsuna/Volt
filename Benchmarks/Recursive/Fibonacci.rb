def fibonacci( n )
    if n <= 0
        0
    elsif n == 1
        1
    else
        fibonacci( n - 1 ) + fibonacci( n - 2 )
    end
end

target = 35
result = fibonacci target
puts "Fibonacci of #{target} is #{result}"
