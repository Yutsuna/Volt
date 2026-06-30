<?php
function fibonacci( int $n ): int
{
    if ($n <= 0) {
        return 0;
    } elseif ($n === 1) {
        return 1;
    }
    return fibonacci( $n - 1 ) + fibonacci( $n - 2 );
}

$target = 35;
$result = fibonacci( $target );
echo "Fibonacci of $target is $result\n";
?>
