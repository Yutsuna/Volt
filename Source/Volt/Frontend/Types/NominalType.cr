module Volt::Frontend


  # A resolved reference to a user-defined nominal type : a `class` instance
  # (`TypeKind::Object`) or a `struct` value (`TypeKind::Struct`). It carries the
  # declared name, the registry `type_id`, and (once Phase B has run) its packed
  # `TypeLayout`. Equality is by name so two references to the same type unify
  # regardless of which annotation site produced them.
  class NominalType < Type
    property name    : String
    property type_id : Int32
    property layout  : TypeLayout?
    # Register-slot layout for the Tier-0 VM (architecture #1.B): one `Value`
    # slot per primitive/class-reference field, N slots for a nested struct
    # field (N = that struct's own slot count). Distinct from `layout` (real
    # C-ABI byte packing, kept for the future native/Cranelift backend) :
    # every slot here is uniformly width-1, so no alignment/padding applies.
    property reg_layout : TypeLayout?

    def initialize( @name : String, kind : TypeKind, @type_id : Int32 = -1,
                    @layout : TypeLayout? = nil, @reg_layout : TypeLayout? = nil )
      super( kind )
    end

    def self.object( name : String, type_id : Int32 = -1, layout : TypeLayout? = nil ) : NominalType
      new( name, TypeKind::Object, type_id, layout )
    end

    def self.struct( name : String, type_id : Int32 = -1, layout : TypeLayout? = nil ) : NominalType
      new( name, TypeKind::Struct, type_id, layout )
    end

    def ==( other : Type ) : Bool
      return false unless other.is_a?( NominalType )
      kind == other.kind && name == other.name
    end

    def to_s( io : IO ) : Nil
      io << name
    end

    # A `class` instance is always a heap pointer (8 bytes). A `struct` value's
    # footprint is its own packed layout : falls back to the pointer-sized
    # default only while the layout hasn't been computed yet (Phase B running).
    def byte_size : Int32
      if kind.struct? && ( l = layout )
        l.total_size
      else
        super
      end
    end

    def alignment : Int32
      if kind.struct? && ( l = layout )
        l.align
      else
        super
      end
    end
  end


end
