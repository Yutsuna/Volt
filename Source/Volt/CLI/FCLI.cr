module Volt
  module CLI


    module FCLI

      extend self

      def run ( args : Array(String) ) : Nil
        case args[0]?
        when "build"   then cmd_build(args[1..])
        when "run"     then cmd_run(args[1..])
        when "version" then FLog.info "Volt #{VERSION}"
        else                help
        end
      end

      #--------------------------------------------------------------------------

      private def cmd_build ( args : Array(String) ) : Nil
        FLog.step "Building..."
        file = positional( args )
        return missing_file unless file
        FLog.info "  Compiling #{file}..."
        output = option( args, "-o" ) || default_output( file )
        ok = Driver::FDriver.new( file ).compile( output )
        if ok
          FLog.ok "Compiled #{file} -> #{output}"
        else
          exit 1
        end
      end

      private def cmd_run ( args : Array(String) ) : Nil
        file = positional( args )
        return missing_file unless file
        exit Driver::FDriver.new( file ).run
      end

      #--------------------------------------------------------------------------

      private def positional ( args : Array(String) ) : String?
        args.find { |a| !a.starts_with?( '-' ) }
      end

      private def option ( args : Array(String), flag : String ) : String?
        if idx = args.index( flag )
          args[idx + 1]?
        end
      end

      private def default_output ( file : String ) : String
        base = File.basename( file )
        ext  = File.extname( base )
        ext.empty? ? "#{base}.out" : base[ 0...( base.size - ext.size ) ]
      end

      private def missing_file : Nil
        FLog.error "no input file given"
        exit 1
      end


      private def help : Nil
        FLog.info("Volt #{VERSION}")
        FLog.info("usage:")
        FLog.info("  volt build <file.vl> [-o <output>]")
        FLog.info("  volt run   <file.vl>")
        FLog.info("  volt version")
      end

    end


  end
end
