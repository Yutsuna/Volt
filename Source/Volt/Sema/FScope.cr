module Volt
  module Sema

    class FScope
      getter parent : FScope?

      def initialize(@parent : FScope? = nil)
        @vars     = {} of String => {String, Types::Type}
        @versions = {} of String => Int32
      end

      def lookup ( name : String ) : {String, Types::Type}?
        @vars[name]? || @parent?.try(&.lookup(name))
      end

      def assign ( name : String, type : Types::Type ) : String
        if existing = lookup(name)
          return existing[0] if existing[1] == type
        end

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
