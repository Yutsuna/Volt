<?php


class Vector3 {
    public float $x;
    public float $y;
    public float $z;

    public function __construct(float $x, float $y, float $z) {
        $this->x = $x;
        $this->y = $y;
        $this->z = $z;
    }

    public function magnitude_squared(): float {
        return $this->x * $this->x + $this->y * $this->y + $this->z * $this->z;
    }

    public function add(Vector3 $other): Vector3 {
        return new Vector3($this->x + $other->x, $this->y + $other->y, $this->z + $other->z);
    }
}


trait Loggable {
    public int $log_count = 0;
    public function log_message(string $message): void {
        $this->log_count++;
    }
    public function get_log_count(): int {
        return $this->log_count;
    }
}


abstract class Shape {
    use Loggable;
    public float $x;
    public float $y;

    public function __construct(float $x, float $y) {
        $this->x = $x;
        $this->y = $y;
    }

    abstract public function area(): float;
    abstract public function perimeter(): float;
}


class Circle extends Shape {
    public float $radius;

    public function __construct(float $x, float $y, float $radius) {
        parent::__construct($x, $y);
        $this->radius = $radius;
    }

    public function area(): float {
        return 3.141592653589793 * $this->radius * $this->radius;
    }

    public function perimeter(): float {
        return 2.0 * 3.141592653589793 * $this->radius;
    }
}


class Rectangle extends Shape {
    public float $width;
    public float $height;

    public function __construct(float $x, float $y, float $width, float $height) {
        parent::__construct($x, $y);
        $this->width = $width;
        $this->height = $height;
    }

    public function area(): float {
        return $this->width * $this->height;
    }

    public function perimeter(): float {
        return 2.0 * ($this->width + $this->height);
    }
}


class FileHandle {
    public string $path;
    public bool $is_open;

    public function __construct(string $path) {
        $this->path = $path;
        $this->is_open = true;
    }

    public function read(): string {
        return $this->is_open ? "Content" : "Closed";
    }

    public function close(): void {
        $this->is_open = false;
    }

    public function __destruct() {
        $this->close();
    }
}


echo "Running Struct Benchmark...\n";
$v1 = new Vector3(1.0, 2.0, 3.0);
$v2 = new Vector3(4.0, 5.0, 6.0);
$sum_struct = 0.0;
$i = 0;
while ($i < 10000000) {
    $res = $v1->add($v2);
    $sum_struct += $res->magnitude_squared();
    $i++;
}
echo "Struct: " . $sum_struct . "\n";

$sum_area = 0.0;
$j = 0;
while ($j < 5000000) {
    if ($j % 2 == 0) {
        $shape = new Circle(0.0, 0.0, 5.0);
    } else {
        $shape = new Rectangle(0.0, 0.0, 10.0, 20.0);
    }
    $sum_area += $shape->area();
    $j++;
}
echo "Polymorphism: " . $sum_area . "\n";

$k = 0;
while ($k < 3000000) {
    $file = new FileHandle("/tmp/test.txt");
    $file->read();
    $k++;
}
echo "RAII Completed.\n";
