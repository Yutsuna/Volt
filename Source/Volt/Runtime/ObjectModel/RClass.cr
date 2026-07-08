module Volt::Runtime::ObjectModel


  # Runtime metadata for one compiled class/struct : populated by the
  # compiler (from `Frontend::TypeInfo`) and consulted by the VM at
  # allocation and drop time. `finalize_index`/`drop_fields_index` are chunk
  # indices into `Compiler::Unit#chunks`, or -1 when absent (a type with no
  # `finalize` and no reference fields needs neither).
  class RClass
    property type_id          : Int32
    property name             : String
    property slot_count       : Int32
    property finalize_index   : Int32
    property drop_fields_index : Int32
    property dtor_index       : Int32
    # Chunk index per vtable slot (architecture #7.3) — a `CALL_METHOD`'s `B`
    # operand indexes straight into this array. `-1` for a slot with no
    # implementation reachable on this class (should not occur for a
    # concrete, non-abstract class : `Semantic` rejects incomplete ones).
    property vtable : Array( Int32 )

    def initialize( @type_id : Int32, @name : String, @slot_count : Int32,
                    @finalize_index : Int32 = -1, @drop_fields_index : Int32 = -1,
                    @dtor_index : Int32 = -1, @vtable = [] of Int32 )
    end
  end


end
