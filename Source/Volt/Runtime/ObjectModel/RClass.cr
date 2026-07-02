module Volt::Runtime::ObjectModel


  # Runtime metadata for one compiled class/struct : populated by the
  # compiler (from `Frontend::TypeInfo`) and consulted by the VM at
  # allocation and drop time. `finalize_index`/`drop_fields_index` are chunk
  # indices into `Compiler::Unit#chunks`, or -1 when absent (a type with no
  # `finalize` and no reference fields needs neither).
  class RClass
    property type_id          : Int32
    property name              : String
    property slot_count        : Int32
    property finalize_index    : Int32
    property drop_fields_index : Int32

    def initialize( @type_id : Int32, @name : String, @slot_count : Int32,
                    @finalize_index : Int32 = -1, @drop_fields_index : Int32 = -1 )
    end
  end


end
