module Volt::Runtime::ObjectModel


  # `type_id -> RClass` lookup table, built once from `Compiler::Unit#classes`
  # when the VM starts.
  class TypeRegistry
    def initialize( classes : Array( RClass ) = [] of RClass )
      @by_id = {} of Int32 => RClass
      classes.each { |c| register( c ) }
    end

    def register( c : RClass ) : Nil
      @by_id[ c.type_id ] = c
    end

    def []( id : Int32 ) : RClass
      @by_id[ id ]
    end

    def []?( id : Int32 ) : RClass?
      @by_id[ id ]?
    end

    # Linear scan, only ever used for a one-time, memoised lookup (`Vm` caches
    # the resolved `type_id`) — never on a hot path.
    def find_by_name( name : String ) : RClass?
      @by_id.each_value { |c| return c if c.name == name }
      nil
    end
  end


end
