require "./EParseError"
require "./FParse"

module Volt
  module CLI


    module FCommands

      alias CommandArgs = NamedTuple(
        command: String?,
        file: String?,
        output: String?,
        verbose: Bool
      )

      # -------------------------------------------------------------------------

      def self.execute!( args ) : Int32
        args_tuple = parse!( args )
        dispatch!( args_tuple[:command], args_tuple )
      end

      # -------------------------------------------------------------------------

      def self.parse!( args : Array(String) ) : CommandArgs
        raise EParseError.new( "No command provided" ) if args.empty?
        command = args[ 0 ]?
        unless COMMANDS.includes? command
          raise EParseError.new "Unknown command: #{command.inspect}. Use 'volt help' for usage."
        end
        {
          command:   command,
          file:      FParse.file(args),
          output:    FParse.option(args, "-o", "--output"),
          verbose:   FParse.flag(args, "-v", "--verbose"),
        }
      end

      def self.dispatch!( command, args : CommandArgs ) : Int32
        # On récupère le pointeur de fonction depuis la map
        if proc = COMMANDS_MAP[ command ]?
          # On l'appelle via .call
          proc.call( args )
        else
          raise EParseError.new "Unknown command: #{command.inspect}. Use 'volt help' for usage."
        end
      end

      # -------------------------------------------------------------------------

      def self.build( args : CommandArgs ) : Int32
        FLog.verbose(args[:verbose])

        file = args[:file]? || raise EParseError.new("Missing input file for `build`")
        FLog.step("Compiling #{file}...")

        output = args[:output]? || default_output(file)
        ok = Driver::FDriver.new(file).compile(output)

        if ok
          FLog.ok("Compiled #{file} -> #{output}")
          0
        else
          1
        end
      end

      def self.run( args : CommandArgs ) : Int32
        file = args[:file]? || raise EParseError.new("Missing input file for `run`")
        Driver::FDriver.new(file).run
      end

      def self.version(args : CommandArgs) : Int32
        FLog.info("Volt #{Volt::VERSION}")
        0
      end

      def self.help(args : CommandArgs) : Int32
        FLog.info("Volt #{Volt::VERSION}")
        FLog.info("usage:")
        FLog.info("  volt build <file#{EXTENSION}> [-o <output>] [-v|--verbose]")
        FLog.info("  volt run   <file#{EXTENSION}>")
        FLog.info("  volt version")
        0
      end

      def self.default_output(file : String) : String
        base = File.basename file
        ext = File.extname base
        ext.empty? ? "#{base}.out" : base[ 0...( base.size - ext.size ) ]
      end

      # -------------------------------------------------------------------------

      COMMANDS_MAP = {
        "build"   => ->self.build(CommandArgs),
        "run"     => ->self.run(CommandArgs),
        "version" => ->self.version(CommandArgs),
        "help"    => ->self.help(CommandArgs),
      }

      COMMANDS = COMMANDS_MAP.keys.to_set
      EXTENSION = ".vl"

    end
  end
end
