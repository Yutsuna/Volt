class Vector3:
    __slots__ = ['x', 'y', 'z']
    def __init__(self, x: float, y: float, z: float):
        self.x = x
        self.y = y
        self.z = z

    def magnitude_squared(self) -> float:
        return self.x * self.x + self.y * self.y + self.z * self.z

    def add(self, other: 'Vector3') -> 'Vector3':
        return Vector3(self.x + other.x, self.y + other.y, self.z + other.z)


class Loggable:
    def __init__(self):
        self.log_count = 0
    def log_message(self, message: str) -> None:
        self.log_count += 1
    def get_log_count(self) -> int:
        return self.log_count


class Shape(Loggable):
    def __init__(self, x: float, y: float):
        super().__init__()
        self.x = x
        self.y = y


class Circle(Shape):
    def __init__(self, x: float, y: float, radius: float):
        super().__init__(x, y)
        self.radius = radius

    def area(self) -> float:
        return 3.141592653589793 * self.radius * self.radius

    def perimeter(self) -> float:
        return 2.0 * 3.141592653589793 * self.radius


class Rectangle(Shape):
    def __init__(self, x: float, y: float, width: float, height: float):
        super().__init__(x, y)
        self.width = width
        self.height = height

    def area(self) -> float:
        return self.width * self.height

    def perimeter(self) -> float:
        return 2.0 * (self.width + self.height)


class FileHandle:
    def __init__(self, path: str):
        self.path = path
        self.is_open = True

    def read(self) -> str:
        return "Content" if self.is_open else "Closed"

    def close(self) -> None:
        self.is_open = False

    def finalize(self) -> None:
        self.close()


v1 = Vector3(1.0, 2.0, 3.0)
v2 = Vector3(4.0, 5.0, 6.0)
sum_struct = 0.0
i = 0
while i < 10000000:
    res = v1.add(v2)
    sum_struct += res.magnitude_squared()
    i += 1
print(f"Struct: {sum_struct}")

sum_area = 0.0
j = 0
while j < 5000000:
    if j % 2 == 0:
        shape = Circle(0.0, 0.0, 5.0)
    else:
        shape = Rectangle(0.0, 0.0, 10.0, 20.0)
    sum_area += shape.area()
    j += 1
print(f"Polymorphism: {sum_area}")

k = 0
while k < 3000000:
    file = FileHandle("/tmp/test.txt")
    file.read()
    file.finalize()
    k += 1
print("RAII Completed.")
