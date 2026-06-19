require "./EParseError"

module Volt
  module CLI


    # -------------------------------------------------------------------------

    alias CommandArgs = NamedTuple(
      command: String?,
      file: String?,
      output: String?,
      verbose: Bool
    )

    # -------------------------------------------------------------------------


    module FCommands


      # -------------------------------------------------------------------------

      def self.execute!( args ) : Int32
        args_tuple = parse!( args )
        dispatch!( args_tuple[ :command ], args_tuple )
      end

      # -------------------------------------------------------------------------

      def self.parse!( args : Array(String) ) : CommandArgs
        raise EParseError.new( "No command provided." ) if args.empty?
        command = args[  0  ]?
        unless COMMANDS.includes? command
          raise EParseError.new "Unknown command: #{command.inspect}."
        end
        {
          command:   command,
          file:      FParse.file( args ),
          output:    FParse.option( args, "-o", "--output"),
          verbose:   FParse.flag( args, "-v", "--verbose"),
        }
      end

      def self.dispatch!( command, args : CommandArgs ) : Int32
        if proc = COMMANDS_MAP[ command ]?
          proc.call( args )
        else
          raise EParseError.new "Unknown command: #{command.inspect}."
        end
      end

      # -------------------------------------------------------------------------

      def self.build( args : CommandArgs ) : Int32
        FLog.verbose(args[ :verbose ])

        file = args[ :file ]? || raise EParseError.new "Missing input file for `build`"
        FLog.step "Compiling #{file}..."

        output = args[ :output ]? || default_output( file )
        if Driver::FDriver.new( file ).compile( output )
          FLog.ok("Compiled #{file} -> #{output}")
          0
        else
          1
        end
      end

      def self.run( args : CommandArgs ) : Int32
        file = args[ :file ]? || raise EParseError.new "Missing input file for `run`"
        Driver::FDriver.new( file ).run
      end

      def self.version( args : CommandArgs ) : Int32
        FLog.info("Volt #{Volt::VERSION}")
        0
      end

      def self.help( args : CommandArgs ) : Int32
        FLog.info("Volt #{Volt::VERSION}")
        FLog.info("usage:")
        FLog.info("  volt build <file#{EXTENSION}> [-o <output>] [-v|--verbose]")
        FLog.info("  volt run   <file#{EXTENSION}>")
        FLog.info("  volt version")
        0
      end

      def self.default_output( file : String ) : String
        base = File.basename file
        ext = File.extname base
        ext.empty? ? "#{base}.out" : base[ 0...( base.size - ext.size ) ]
      end

      #--------------------------------------------------------------------------

    end

    # -------------------------------------------------------------------------

    COMMANDS_MAP = {
      "build"   => ->FCommands.build( CommandArgs ),
      "run"     => ->FCommands.run( CommandArgs ),
      "version" => ->FCommands.version( CommandArgs ),
      "help"    => ->FCommands.help( CommandArgs ),
    }

    COMMANDS = COMMANDS_MAP.keys.to_set
    EXTENSION = ".vl"

    # -------------------------------------------------------------------------

    module FParse

      extend self

      #--------------------------------------------------------------------------

      def file( args : Array(String) ) : String?
        args.each { |arg| next if arg.starts_with?( '-' ) || COMMANDS.includes?( arg ) ; return arg }
        nil
      end

      def option( args : Array(String), short : String, long : String ) : String?
        index = args.index( short ) || args.index( long )
        return nil unless index
        args[ index  + 1]
      end

      def flag( args : Array(String), *flags : String ) : Bool
        flags.any? { |f| args.includes? f }
      end

      #--------------------------------------------------------------------------

    end


  end
end
