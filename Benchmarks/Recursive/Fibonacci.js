function fibonacci(n)
{
    if (n <= 0) {
        return 0;
    } else if (n == 1) {
        return 1;
    } else {
        return fibonacci( n - 1) + fibonacci(n - 2);
    }
}

target = 35;
result = fibonacci(target);
console.log( `Fibonacci of ${target} is: ${result}` );
