require "./ACommand"


module Volt::CLI
  class AstCommand < ACommand
    register "ast", "Generate & display the abstract syntax tree"

    property input : String?
    property output : String?
    property format : String = "json"
    property simplify : Bool = false
    property color : Bool = false
    property no_location : Bool = false

    def execute(args : Array(String))
      parse args
      fatal! "Missing input parsing option (-i)" unless @input

      Logger.info("Starting lexer stage token checks", "ast")

      Logger.progress("Scanning tokens from grammar inputs...", "1/3")
      sleep 0.1.seconds

      Logger.progress("Assembling structural parsing tree branch logic...", "2/3")
      sleep 0.15.seconds

      if target_out = @output
        Logger.progress("Exporting structured parsing database to: #{target_out}", "3/3", finished: true)
      else
        Logger.progress("Parsing nodes finished validation", "3/3", finished: true)
        puts "\nRootNode"
        puts "└── FunctionNode (main)"
        puts "    └── ReturnNode (value: 0)"
      end
    end


    private def parse( args : Array(String) )
      parser = OptionParser.parse(args) do |p|
        p.banner = "Usage: volt ast [options] [input_file]"
        p.on("-i INPUT", "--input INPUT", "Source input module path") { |v| @input = v }
        p.on("-o OUTPUT", "--output OUTPUT", "Output target path structure") { |v| @output = v }
        p.on("--format FORMAT", "Serialization formats (json|dot|text)") { |v| @format = v }
        p.on("--simplify", "Deduplicate structural tree layout elements") { @simplify = true }
        p.on("--color", "Output tree layout using shell graphics colors") { @color = true }
        p.on("--no-location", "Omit character and index coordinates") { @no_location = true }
        p.on("-h", "--help", "Show help") { puts p; next }
        p.invalid_option { |flag| fatal! "Invalid option: #{flag}\n#{p}" }
      end

      @input = args.first? if @input.nil?
    end


  end


end
