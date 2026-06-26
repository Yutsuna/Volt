require "./ACommand"


module Volt::CLI
  class AstCommand < ACommand
    register "ast", "Generate & display the abstract syntax tree"

    property input : String?
    property output : String?
    property format : String = "json"
    property simplify : Bool = false
    property no_color : Bool = false
    property no_location : Bool = false

    def execute(args : Array(String))
      parse args
      fatal! "Missing input file" unless @input

      input_path = @input.not_nil!
      source = begin
        File.read(input_path)
      rescue e
        fatal! "Cannot read '#{input_path}': #{e.message}"
      end

      Frontend::ASTDump.show_loc = !@no_location
      Frontend::ASTDump.color    = !@no_color && (@output.nil? && STDOUT.tty?)

      program = begin
        Frontend.parse(source, input_path)
      rescue e : Frontend::CompilationError
        DiagnosticRenderer.new( { input_path => source } ).render( e.bag )
        raise SystemExit.new
      end

      if target_out = @output
        File.open(target_out, "w") { |f| program.inspect(f) }
      else
        program.inspect(STDOUT)
      end
    end


    private def parse( args : Array(String) )
      parser = OptionParser.parse(args) do |p|
        p.banner = "Usage: volt ast [options] [input_file]"
        p.on("-i INPUT", "--input INPUT", "Source input module path") { |v| @input = v }
        p.on("-o OUTPUT", "--output OUTPUT", "Output target path structure") { |v| @output = v }
        p.on("--format FORMAT", "Serialization formats (json|dot|text)") { |v| @format = v }
        p.on("--simplify", "Deduplicate structural tree layout elements") { @simplify = true }
        p.on("--no-color", "Output without colors") { @no_color = true }
        p.on("--no-location", "Omit character and index coordinates") { @no_location = true }
        p.on("-h", "--help", "Show help") { puts p; next }
        p.invalid_option { |flag| fatal! "Invalid option: #{flag}\n#{p}" }
      end

      @input = args.first? if @input.nil?
    end


  end


end
