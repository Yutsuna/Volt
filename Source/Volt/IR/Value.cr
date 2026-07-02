module Volt::IR


  # Base heap-allocated object for RAII management (architecture #4).
  #
  # `fields` holds one `Value` slot per register-slot in the owning type's
  # `TypeInfo#reg_layout` : a primitive or class-reference field is one slot,
  # a nested struct field flattens in as N slots. This mirrors how a *local*
  # struct value lives directly in a contiguous run of Frame registers: both
  # are just "N tagged Value slots", one heap-resident and one frame-resident
  # (architecture #1.B : no byte-level packing at this interpreter tier).
  #
  # `class_ref` points at the owning type's runtime metadata
  # (`Runtime::ObjectModel::RClass` : vtable, itables, finalize/drop-fields
  # pointers). It's kept as an opaque `Void*` here rather than typed as
  # `RClass` directly: IR has no dependencies (architecture #10 build order),
  # so the object-model layer casts it back once allocation is wired.
  class HeapObject
    getter type_id    : Int32
    getter? destroyed : Bool
    property fields    : Array( Value )
    property class_ref : Pointer( Void )

    def initialize( @type_id : Int32, slot_count : Int32 = 0 )
      @destroyed = false
      @fields    = Array( Value ).new( slot_count ) { Value.nil_value }
      @class_ref = Pointer( Void ).null
    end

    def destroy
      @destroyed = true
    end
  end


  # A runtime value for the Tier-0 VM.
  # v0.1.0 uses a simple boxed tagged union. The architecture calls for untagged
  # registers + NaN-boxing once the contract is stable : that is a later optimization
  # and must keep this public API (the `as_*` accessors + constructors) intact.
  struct Value
    alias Raw = Int64 | Float64 | Bool | String | ::Regex | Nil | HeapObject

    getter raw : Raw

    def initialize( @raw : Raw )
    end

    def self.int( v : Int64 ) : Value                     ; new( v )    ; end
    def self.float( v : Float64 ) : Value                 ; new( v )    ; end
    def self.bool( v : Bool ) : Value                     ; new( v )    ; end
    def self.str( v : String ) : Value                    ; new( v )    ; end
    def self.regex( v : ::Regex ) : Value                 ; new( v )    ; end
    def self.nil_value : Value                            ; new( nil )  ; end
    def self.object( obj : HeapObject ) : Value           ; new( obj )  ; end

    def as_i : Int64                      ; raw.as( Int64 )           ; end
    def as_f : Float64                    ; raw.as( Float64 )         ; end
    def as_bool : Bool                    ; raw.as( Bool )            ; end
    def as_s : String                     ; raw.as( String )          ; end
    def as_regex : ::Regex                ; raw.as( ::Regex )         ; end
    def as_object : HeapObject            ; raw.as( HeapObject )      ; end
    def is_nil? : Bool
      raw.nil?
    end

    # Volt truthiness: only `nil` and `false` are falsy.
    def truthy? : Bool
      r = raw
      !( r.nil? || r == false )
    end

    # Surface form used by `puts` / `print`.
    def to_display : String
      r = raw
      case r
      when Nil        then "nil"
      when ::Regex    then r.source
      when HeapObject then "<object:#{r.type_id}>"
      else                 r.to_s
      end
    end

    def ==( other : Value ) : Bool
      r1 = raw
      r2 = other.raw
      return r1 == r2 if r1.is_a?( Number ) && r2.is_a?( Number )
      raw == other.raw
    end

  end


end
