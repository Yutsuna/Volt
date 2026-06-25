require "./ACommand"


module Volt::CLI


  class REPLCommand < ACommand
    register "repl", "Read-Eval-Print-Loop"

    property input : String?

    def execute(args : Array(String))
      parse args
      Logger.info("Volt Developer Interactive Loop", "repl")
      if init_path = @input
        Logger.info("Evaluating preloaded module: #{init_path}", "repl")
      end
      Logger.info("Type 'exit' or use Ctrl+D to terminate.", "repl")
    end

    private def parse( args : Array(String) )
      parser = OptionParser.parse(args) do |p|
        p.banner = "Usage: volt repl [options] [preloaded_file]"
        p.on("-i INPUT", "--input INPUT", "File source input to execute at init") { |v| @input = v }
        p.on("-h", "--help", "Show help") { puts p; next }
        p.invalid_option { |flag| fatal! "Invalid option: #{flag}\n#{p}" }
      end

      @input = args.first? if @input.nil?
    end

  end


end
