module Volt::IR


  # A runtime value for the Tier-0 VM.
  # v0.1.0 uses a simple boxed tagged union. The architecture calls for untagged
  # registers + NaN-boxing once the contract is stable — that is a later optimization
  # and must keep this public API (the `as_*` accessors + constructors) intact.
  struct Value
    alias Raw = Int64 | Float64 | Bool | String | Nil

    getter raw : Raw

    def initialize( @raw : Raw )
    end

    def self.int( v : Int64 ) : Value     ; new( v )   ; end
    def self.float( v : Float64 ) : Value ; new( v )   ; end
    def self.bool( v : Bool ) : Value     ; new( v )   ; end
    def self.str( v : String ) : Value    ; new( v )   ; end
    def self.nil_value : Value            ; new( nil ) ; end

    def as_i : Int64    ; raw.as( Int64 )   ; end
    def as_f : Float64  ; raw.as( Float64 ) ; end
    def as_bool : Bool  ; raw.as( Bool )    ; end
    def as_s : String   ; raw.as( String )  ; end

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
      when Nil then "nil"
      else          r.to_s
      end
    end

    def ==( other : Value ) : Bool
      raw == other.raw
    end
  end


end
