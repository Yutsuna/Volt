def fibonacci( n : int ) -> int:
    if n <= 0:
        return 0
    elif n == 1:
        return 1
    else:
        return fibonacci( n - 1 ) + fibonacci( n - 2 )

target = 35
result = fibonacci( target )
print(f"The {target}th Fibonacci number is: {result}", flush=True)
