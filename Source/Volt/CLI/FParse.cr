module Volt
  module CLI


    module FParse

      extend self

      #--------------------------------------------------------------------------

      def file( args : Array(String) ) : String?
        args.each { |arg| next if arg.starts_with?( '-' ) ; return arg }
        nil
      end

      def option( args : Array(String), short : String, long : String ) : String?
        index = args.index( short ) || args.index( long )
        return nil unless index
        args[index + 1]
      end

      def flag(args : Array(String), *flags : String) : Bool
        flags.any? { |f| args.includes? f }
      end

      #--------------------------------------------------------------------------

    end


  end
end
