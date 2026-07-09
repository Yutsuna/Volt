class Vector3 {
    constructor(x, y, z) {
        this.x = x;
        this.y = y;
        this.z = z;
    }
    magnitude_squared() {
        return this.x * this.x + this.y * this.y + this.z * this.z;
    }
    add(other) {
        return new Vector3(this.x + other.x, this.y + other.y, this.z + other.z);
    }
}


class Loggable {
    constructor() {
        this.log_count = 0;
    }
    log_message(message) {
        this.log_count++;
    }
    get_log_count() {
        return this.log_count;
    }
}


class Shape extends Loggable {
    constructor(x, y) {
        super();
        this.x = x;
        this.y = y;
    }
}


class Circle extends Shape {
    constructor(x, y, radius) {
        super(x, y);
        this.radius = radius;
    }
    area() {
        return 3.141592653589793 * this.radius * this.radius;
    }
    perimeter() {
        return 2.0 * 3.141592653589793 * this.radius;
    }
}


class Rectangle extends Shape {
    constructor(x, y, width, height) {
        super(x, y);
        this.width = width;
        this.height = height;
    }
    area() {
        return this.width * this.height;
    }
    perimeter() {
        return 2.0 * (this.width + this.height);
    }
}


class FileHandle {
    constructor(path) {
        this.path = path;
        this.is_open = true;
    }
    read() {
        return this.is_open ? "Content" : "Closed";
    }
    close() {
        this.is_open = false;
    }
    finalize() {
        this.close();
    }
}


let v1 = new Vector3(1.0, 2.0, 3.0);
let v2 = new Vector3(4.0, 5.0, 6.0);
let sum_struct = 0.0;
let i = 0;
while (i < 10_000_000) {
    let res = v1.add(v2);
    sum_struct += res.magnitude_squared();
    i++;
}
console.log("Struct: " + sum_struct);

let sum_area = 0.0;
let j = 0;
while (j < 5_000_000) {
    let shape;
    if (j % 2 === 0) {
        shape = new Circle(0.0, 0.0, 5.0);
    } else {
        shape = new Rectangle(0.0, 0.0, 10.0, 20.0);
    }
    sum_area += shape.area();
    j++;
}
console.log("Polymorphism: " + sum_area);

let k = 0;
while (k < 3_000_000) {
    let file = new FileHandle("/tmp/test.txt");
    file.read();
    file.finalize();
    k++;
}
console.log("RAII Completed.");
