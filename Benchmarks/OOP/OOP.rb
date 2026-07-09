module OOPBench


  class Vector3
    attr_accessor :x, :y, :z

    def initialize(x, y, z)
      @x, @y, @z = x, y, z
    end

    def magnitude_squared
      @x * @x + @y * @y + @z * @z
    end

    def add(other)
      Vector3.new(@x + other.x, @y + other.y, @z + other.z)
    end
  end


  module Loggable
    attr_accessor :log_count

    def init_loggable
      @log_count = 0
    end

    def log_message(message)
      @log_count += 1
    end
  end


  class Shape
    include Loggable
    attr_accessor :x, :y

    def initialize(x, y)
      init_loggable
      @x, @y = x, y
    end
  end


  class Circle < Shape
    attr_accessor :radius

    def initialize(x, y, radius)
      super(x, y)
      @radius = radius
    end

    def area
      3.141592653589793 * @radius * @radius
    end

    def perimeter
      2.0 * 3.141592653589793 * @radius
    end
  end


  class Rectangle < Shape
    attr_accessor :width, :height

    def initialize(x, y, width, height)
      super(x, y)
      @width, @height = width, height
    end

    def area
      @width * @height
    end

    def perimeter
      2.0 * (@width + @height)
    end
  end


  class FileHandle
    def initialize(path)
      @path = path
      @is_open = true
    end

    def read
      @is_open ? "Content" : "Closed"
    end

    def close
      @is_open = false if @is_open
    end

    def finalize
      close
    end
  end


end


v1 = OOPBench::Vector3.new(1.0, 2.0, 3.0)
v2 = OOPBench::Vector3.new(4.0, 5.0, 6.0)
sum_struct = 0.0
i = 0
while i < 10_000_000
  res = v1.add(v2)
  sum_struct += res.magnitude_squared
  i += 1
end
puts "Struct: #{sum_struct}"

sum_area = 0.0
j = 0
while j < 5_000_000
  if j % 2 == 0
    shape = OOPBench::Circle.new(0.0, 0.0, 5.0)
  else
    shape = OOPBench::Rectangle.new(0.0, 0.0, 10.0, 20.0)
  end
  sum_area += shape.area
  j += 1
end
puts "Polymorphism: #{sum_area}"

k = 0
while k < 3_000_000
  file = OOPBench::FileHandle.new("/tmp/test.txt")
  file.read
  file.finalize
  k += 1
end
puts "RAII Completed."
