module Volt::IR


  # Base heap-allocated object for RAII management (architecture #4). Will be replaced
  # by the full object model (Runtime/ObjectModel/) once classes land.
  class HeapObject
    getter type_id : Int32
    getter? destroyed : Bool

    def initialize( @type_id : Int32 )
      @destroyed = false
    end

    def destroy
      @destroyed = true
    end
  end


  # A runtime value for the Tier-0 VM.
  # v0.1.0 uses a simple boxed tagged union. The architecture calls for untagged
  # registers + NaN-boxing once the contract is stable — that is a later optimization
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
