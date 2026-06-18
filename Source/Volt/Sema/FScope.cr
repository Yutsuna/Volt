module Volt
  module Sema


    class FScope

      def initialize
        @vars     = {} of String => {String, Types::Type}
        @versions = {} of String => Int32
      end

      #--------------------------------------------------------------------------

      def lookup ( name : String ) : {String, Types::Type}?
        @vars[name]?
      end

      def assign ( name : String, type : Types::Type ) : String
        existing = @vars[name]?
        return existing[0] if existing && existing[1] == type

        version = @versions[name]? || 0
        slot = version == 0 ? name : "#{name}.#{version}"
        @versions[name] = version + 1
        @vars[name] = {slot, type}
        slot
      end

      def define_param ( name : String, type : Types::Type ) : String
        @vars[name] = {name, type}
        @versions[name] = 1
        name
      end

    end


  end
end
