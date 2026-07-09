local Vector3 = {}
Vector3.__index = Vector3

function Vector3.new(x, y, z)
    local self = setmetatable({}, Vector3)
    self.x = x
    self.y = y
    self.z = z
    return self
end

function Vector3:magnitude_squared()
    return self.x * self.x + self.y * self.y + self.z * self.z
end

function Vector3:add(other)
    return Vector3.new(self.x + other.x, self.y + other.y, self.z + other.z)
end


local Loggable = {}
Loggable.__index = Loggable

function Loggable.init(self)
    self.log_count = 0
end

function Loggable:log_message(message)
    self.log_count = self.log_count + 1
end

function Loggable:get_log_count()
    return self.log_count
end


local Shape = {}
Shape.__index = Shape
setmetatable(Shape, { __index = Loggable })

function Shape.init(self, x, y)
    Loggable.init(self)
    self.x = x
    self.y = y
end


local Circle = {}
Circle.__index = Circle
setmetatable(Circle, { __index = Shape })

function Circle.new(x, y, radius)
    local self = setmetatable({}, Circle)
    Shape.init(self, x, y)
    self.radius = radius
    return self
end

function Circle:area()
    return 3.141592653589793 * self.radius * self.radius
end

function Circle:perimeter()
    return 2.0 * 3.141592653589793 * self.radius
end


local Rectangle = {}
Rectangle.__index = Rectangle
setmetatable(Rectangle, { __index = Shape })

function Rectangle.new(x, y, width, height)
    local self = setmetatable({}, Rectangle)
    Shape.init(self, x, y)
    self.width = width
    self.height = height
    return self
end

function Rectangle:area()
    return self.width * self.height
end

function Rectangle:perimeter()
    return 2.0 * (self.width + self.height)
end


local FileHandle = {}
FileHandle.__index = FileHandle

function FileHandle.new(path)
    local self = setmetatable({}, FileHandle)
    self.path = path
    self.is_open = true
    return self
end

function FileHandle:read()
    if self.is_open then
        return "Content"
    else
        return "Closed"
    end
end

function FileHandle:finalize()
    self.is_open = false
end


local v1 = Vector3.new(1.0, 2.0, 3.0)
local v2 = Vector3.new(4.0, 5.0, 6.0)
local sum_struct = 0.0
local i = 0
while i < 10000000 do
    local res = v1:add(v2)
    sum_struct = sum_struct + res:magnitude_squared()
    i = i + 1
end
print("Struct: " .. sum_struct)


local sum_area = 0.0
local j = 0
while j < 5000000 do
    local shape
    if j % 2 == 0 then
        shape = Circle.new(0.0, 0.0, 5.0)
    else
        shape = Rectangle.new(0.0, 0.0, 10.0, 20.0)
    end
    sum_area = sum_area + shape:area()
    j = j + 1
end
print("Polymorphism: " .. sum_area)


local k = 0
while k < 3000000 do
    local file = FileHandle.new("/tmp/test.txt")
    file:read()
    file:finalize()
    k = k + 1
end
print("RAII Benchmark Completed.")
