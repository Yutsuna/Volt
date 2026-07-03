module Volt::Frontend


  # One packed field inside a class/struct memory block. `offset` is the byte
  # position from the start of the (sub)object; `is_ref` marks fields that hold a
  # heap reference and therefore participate in the auto-generated `__drop_fields`
  # deep-drop (Phase C).
  struct FieldSlot
    property name   : String
    property type   : Type
    property offset : Int32
    property size   : Int32
    property align  : Int32
    property is_ref : Bool

    def initialize( @name : String, @type : Type, @offset : Int32,
                    @size : Int32, @align : Int32, @is_ref : Bool )
    end
  end


  # The resolved memory layout of a nominal type: the C-ABI packing of its fields,
  # the total block size (padded to the alignment) and the alignment itself.
  #
  # `TypeLayout.pack` is a pure, side-effect-free computation so it can be unit
  # tested in isolation. The semantic `LayoutBuilder` pass (Phase B) feeds it the
  # field slots derived from each declaration.
  class TypeLayout
    property fields     : Array( FieldSlot )
    property total_size : Int32
    property align      : Int32

    def initialize( @fields : Array( FieldSlot ), @total_size : Int32, @align : Int32 )
    end

    # Build a field slot from a name and its resolved `Type`, deriving size,
    # alignment and reference-ness. `size`/`align` may be overridden for struct
    # values embedded by value (whose footprint is their own `total_size`).
    def self.slot( name : String, type : Type,
                   size : Int32 = type.byte_size,
                   align : Int32 = type.alignment,
                   is_ref : Bool = type.reference? ) : FieldSlot
      FieldSlot.new( name, type, 0, size, align, is_ref )
    end

    # Pack `slots` (offsets ignored on input) into a contiguous C-ABI layout.
    #
    # When `base` is given, the new layout *inherits* its fields verbatim : the
    # shared prefix guarantees a derived instance reads inherited fields at the
    # exact same offsets as the base, which is what virtual dispatch relies on.
    def self.pack( slots : Array( FieldSlot ), base : TypeLayout? = nil ) : TypeLayout
      packed    = [] of FieldSlot
      offset    = 0
      max_align = 1

      if base
        base.fields.each { |f| packed << f }
        offset    = base.total_size
        max_align = base.align
      end

      slots.each do |s|
        a         = s.align < 1 ? 1 : s.align
        max_align = a if a > max_align
        offset    = align_up( offset, a )
        packed << FieldSlot.new( s.name, s.type, offset, s.size, a, s.is_ref )
        offset += s.size
      end

      TypeLayout.new( packed, align_up( offset, max_align ), max_align )
    end

    # Look up a packed field by name (including inherited fields), or nil.
    def field?( name : String ) : FieldSlot?
      @fields.find { |f| f.name == name }
    end

    # Round `value` up to the next multiple of `align` (a power of two ≥ 1).
    def self.align_up( value : Int32, align : Int32 ) : Int32
      return value if align <= 1
      ( ( value + align - 1 ) // align ) * align
    end
  end


end
