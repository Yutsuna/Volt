require "./Unit"


module Volt::Compiler


  class NativeTable
    getter natives : Array( NativeFunc )

    def initialize
      @natives = [] of NativeFunc
      @index = {} of NativeFunc => Int32
    end

    def intern( libr : String?, name : String ) : Int32
      func = NativeFunc.new(libr, name)
      @index[ func ] ||= begin
        @natives << func
        @natives.size - 1
      end
    end
  end


end
