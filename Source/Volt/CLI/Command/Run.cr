require "./ACommand"


module Volt::CLI


  class RunCommand < ACommand
    register "run", "Interpret the file input"

    property input : String?
    property program_args = [] of String

    def execute(args : Array(String))
      parse args
      fatal! "Missing input file parameter (-i)" unless @input

      file = @input.not_nil!
      unless @program_args.empty?
        # exec the program with args
      end
    end

    private def parse( args : Array(String) )
      if args_index = args.index("--")
        @program_args = args[(args_index + 1)..-1]
        args = args[0...args_index]
      end

      parser = OptionParser.parse(args) do |p|
        p.banner = "Usage: volt run [options] [input_file] [-- ...]"
        p.on("-i INPUT", "--input INPUT", "File input source program") { |v| @input = v }
        p.on("-h", "--help", "Show help") { puts p; next }
        p.invalid_option { |flag| fatal! "Invalid option: #{flag}\n#{p}" }
      end

      @input = args.first? if @input.nil?
    end

  end


end
