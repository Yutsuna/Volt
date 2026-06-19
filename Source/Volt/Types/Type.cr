module Volt
  module Types


    # A value type: a primitive base plus a pointer indirection depth.
    # `Int32` is `{ Int32, 0 }`, `UInt8*` is `{ UInt8, 1 }`.
    struct Type

      getter base : EType
      getter pointer_depth : Int32

      #--------------------------------------------------------------------------

      NAMES = {
        "Void"    => EType::Void,
        "Nil"     => EType::Nil,
        "Bool"    => EType::Bool,
        "Char"    => EType::Char,
        "Int8"    => EType::Int8,
        "Int16"   => EType::Int16,
        "Int32"   => EType::Int32,
        "Int64"   => EType::Int64,
        "UInt8"   => EType::UInt8,
        "UInt16"  => EType::UInt16,
        "UInt32"  => EType::UInt32,
        "UInt64"  => EType::UInt64,
        "Float32" => EType::Float32,
        "Float64" => EType::Float64,
      }

      INTEGERS = {
        EType::Int8, EType::Int16, EType::Int32, EType::Int64,
        EType::UInt8, EType::UInt16, EType::UInt32, EType::UInt64
      }
      FLOATS   = {
        EType::Float32, EType::Float64
      }
      VOIDS    = {
        EType::Void, EType::Nil
      }

      #--------------------------------------------------------------------------

      def initialize ( @base : EType, @pointer_depth : Int32 = 0 )
      end

      def == ( other : Type ) : Bool
        @base == other.base && @pointer_depth == other.pointer_depth
      end

      def self.named ( name : String ) : Type?
        if base = NAMES[ name ]?
          Type.new( base )
        end
      end

      #--------------------------------------------------------------------------

      def pointer? : Bool
        @pointer_depth > 0
      end

      def to_pointer : Type
        Type.new( @base, @pointer_depth + 1 )
      end

      def integer? : Bool
        return false if pointer?
        INTEGERS.includes?( @base )
      end

      def float? : Bool
        return false if pointer?
        FLOATS.includes?( @base )
      end

      def void? : Bool
        return false if pointer?
        VOIDS.includes?( @base )
      end

      #--------------------------------------------------------------------------

      def to_s ( io : IO ) : Nil
        io << base_name
        @pointer_depth.times { io << '*' }
      end

      def base_name : String
        case @base
        when EType::Void    then "Void"
        when EType::Nil     then "Nil"
        when EType::Bool    then "Bool"
        when EType::Char    then "Char"
        when EType::Int8    then "Int8"
        when EType::Int16   then "Int16"
        when EType::Int32   then "Int32"
        when EType::Int64   then "Int64"
        when EType::UInt8   then "UInt8"
        when EType::UInt16  then "UInt16"
        when EType::UInt32  then "UInt32"
        when EType::UInt64  then "UInt64"
        when EType::Float32 then "Float32"
        when EType::Float64 then "Float64"
        else                     "Void"
        end
      end

      #--------------------------------------------------------------------------

      # LLVM IR type string, e.g. "i32", "double", "i8*".
      def llvm : String
        return "#{base_llvm}#{"*" * @pointer_depth}" if pointer?
        base_llvm
      end

      def base_llvm : String
        case @base
        when EType::Bool                              then "i1"
        when EType::Char, EType::Int8, EType::UInt8   then "i8"
        when EType::Int16, EType::UInt16              then "i16"
        when EType::Int32, EType::UInt32              then "i32"
        when EType::Int64, EType::UInt64              then "i64"
        when EType::Float32                           then "float"
        when EType::Float64                           then "double"
        else                                               "void"
        end
      end

    end


  end
end
