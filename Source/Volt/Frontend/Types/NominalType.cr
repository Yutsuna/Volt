module Volt::Frontend


  # A resolved reference to a user-defined nominal type — a `class` instance
  # (`TypeKind::Object`) or a `struct` value (`TypeKind::Struct`). It carries the
  # declared name, the registry `type_id`, and (once Phase B has run) its packed
  # `TypeLayout`. Equality is by name so two references to the same type unify
  # regardless of which annotation site produced them.
  class NominalType < Type
    property name    : String
    property type_id : Int32
    property layout  : TypeLayout?

    def initialize( @name : String, kind : TypeKind, @type_id : Int32 = -1, @layout : TypeLayout? = nil )
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
  end


end
