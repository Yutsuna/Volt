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
  struct HeapObject
    HEADER_BYTES = 16

    def initialize( @ptr : Pointer( Void ) )
    end

    def self.allocate( type_id : Int32, slot_count : Int32 ) : HeapObject
      bytes = HEADER_BYTES + slot_count * sizeof( Value )
      ptr   = GC.malloc( bytes.to_u64 )
      ptr.as( Int32* ).value = type_id
      ( ptr + 4 ).as( UInt8* ).value = 0_u8
      ( ptr + 8 ).as( Pointer( Void )* ).value = Pointer( Void ).null

      fields_ptr = ( ptr + HEADER_BYTES ).as( Pointer( Value ) )
      slot_count.times do |i|
        fields_ptr[ i ] = Value.nil_value
      end

      new( ptr )
    end

    def to_unsafe  : Pointer( Void )         ; @ptr                                      ; end
    def type_id    : Int32                  ; @ptr.as( Int32* ).value                   ; end
    def destroyed? : Bool                   ; ( @ptr + 4 ).as( UInt8* ).value != 0      ; end
    def destroyed  : Bool                   ; destroyed?                                ; end
    def class_ref  : Pointer( Void )         ; ( @ptr + 8 ).as( Pointer( Void )* ).value ; end

    def class_ref=( val : Pointer( Void ) )
      ( @ptr + 8 ).as( Pointer( Void )* ).value = val
    end

    def destroy : Nil
      ( @ptr + 4 ).as( UInt8* ).value = 1_u8
    end

    def fields : Pointer( Value )
      ( @ptr + HEADER_BYTES ).as( Pointer( Value ) )
    end
  end


  # A runtime value for the Tier-0 VM.
  #
  # Phase 4 (architecture #9 perf plan): explicit `{tag, payload}` instead of a
  # Crystal mixed union. `payload` holds the exact bit pattern of the value —
  # for `String`/`::Regex`/`HeapObject` this is the reference's raw undisguised
  # pointer, never OR'd with tag bits (unlike NaN-boxing, which was rejected:
  # disguising the pointer corrupts the address the scanner would otherwise
  # recognize). Always 16 bytes (1 tag byte + padding + 8 payload bytes), same
  # as the prior union representation, but `as_*` accessors are unsafe
  # bit-reinterprets instead of Crystal's runtime union type-checks.
  #
  # CRITICAL — `payload` MUST be typed `Pointer(Void)`, never `UInt64`. Boehm
  # only scans memory that was *registered for scanning*, and Crystal decides
  # that per allocation from the element type's fields: a struct with no
  # pointer-typed field is "atomic" and every container of it
  # (`Pointer(Value).malloc` for `Vm@stack`, `Array(Value)` buffers for
  # `HeapObject#fields` / chunk constants) is allocated with
  # `GC_malloc_atomic` — *unscanned*. With a `UInt64` payload, any
  # String/Regex/HeapObject reachable only through a `Value` was invisible to
  # the GC and collected while still live (manifested as: regex matches
  # segfaulting inside pcre2, interpolated strings going empty, object fields
  # resetting — all only once enough allocation pressure triggered a
  # collection, which is why Int-only benchmarks never noticed). Typing the
  # payload as a real pointer keeps the struct pointer-bearing, so every
  # container of Values is allocated scanned; non-pointer payloads (Int/
  # Float/Bool) just ride along as harmless fake pointers the conservative
  # scanner ignores or, at worst, falsely retains.
  struct Value
    enum Tag : UInt8
      Int
      Float
      Bool
      Str
      Regex
      Nil
      Object
    end

    getter tag : Tag
    @payload : Pointer( Void )

    private def initialize( @tag : Tag, @payload : Pointer( Void ) )
    end

    def self.int( v : Int64 ) : Value           ; new( Tag::Int, v.unsafe_as( Pointer( Void ) ) )       ; end
    def self.float( v : Float64 ) : Value       ; new( Tag::Float, v.unsafe_as( Pointer( Void ) ) )     ; end
    def self.bool( v : Bool ) : Value           ; new( Tag::Bool, Pointer( Void ).new( v ? 1_u64 : 0_u64 ) ) ; end
    def self.str( v : String ) : Value          ; new( Tag::Str, v.unsafe_as( Pointer( Void ) ) )       ; end
    def self.regex( v : ::Regex ) : Value       ; new( Tag::Regex, v.unsafe_as( Pointer( Void ) ) )     ; end
    def self.nil_value : Value                  ; new( Tag::Nil, Pointer( Void ).null )                 ; end
    def self.object( obj : HeapObject ) : Value ; new( Tag::Object, obj.to_unsafe )                     ; end

    def as_i : Int64                      ; @payload.unsafe_as( Int64 )               ; end
    def as_f : Float64                    ; @payload.unsafe_as( Float64 )             ; end
    def as_bool : Bool                    ; !@payload.null?                           ; end
    def as_s : String                     ; @payload.unsafe_as( String )              ; end
    def as_regex : ::Regex                ; @payload.unsafe_as( ::Regex )             ; end
    def as_object : HeapObject            ; HeapObject.new( @payload )                ; end
    def is_nil? : Bool
      @tag == Tag::Nil
    end

    # Volt truthiness: only `nil` and `false` are falsy.
    def truthy? : Bool
      !( @tag == Tag::Nil || ( @tag == Tag::Bool && @payload.null? ) )
    end

    # Surface form used by `puts` / `print`.
    def to_display : String
      case @tag
      when Tag::Nil    then "nil"
      when Tag::Regex  then as_regex.source
      when Tag::Object then "<object:#{as_object.type_id}>"
      when Tag::Str    then as_s
      when Tag::Bool   then as_bool.to_s
      when Tag::Float  then as_f.to_s
      else                  as_i.to_s
      end
    end

    def ==( other : Value ) : Bool
      t1 = @tag
      t2 = other.tag
      numeric1 = t1 == Tag::Int || t1 == Tag::Float
      numeric2 = t2 == Tag::Int || t2 == Tag::Float
      if numeric1 && numeric2
        if t1 == Tag::Int && t2 == Tag::Int
          return as_i == other.as_i
        end
        n1 = t1 == Tag::Int ? as_i.to_f64 : as_f
        n2 = t2 == Tag::Int ? other.as_i.to_f64 : other.as_f
        return n1 == n2
      end
      return false unless t1 == t2
      case t1
      when Tag::Bool   then as_bool   == other.as_bool
      when Tag::Str    then as_s      == other.as_s
      when Tag::Regex  then as_regex  == other.as_regex
      when Tag::Object then as_object == other.as_object
      else                  true # Nil == Nil
      end
    end

  end


end
