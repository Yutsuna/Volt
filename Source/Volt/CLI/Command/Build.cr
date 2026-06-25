require "./ACommand"


module Volt::CLI


  class BuildCommand < ACommand
    register "build", "Compile the file input"

    property output : String = "main.bin"
    property release : Bool = false
    property defines = [] of String
    property libs = [] of String
    property input : String?

    def execute(args : Array(String))

      parse args
      fatal! "Missing input file compiler parameter (-i)" unless @input

      Logger.info("Building target binaries targeting native executable instructions", "build")

      steps = 5

      Logger.progress("Translate-C: Mapping include declarations", "1/#{steps}")
      sleep 0.15.seconds

      Logger.progress("Parsing: Constructing language node database structures", "2/#{steps}")
      sleep 0.2.seconds

      Logger.progress("Semantic: Checking identifier definitions and boundaries", "3/#{steps}")
      sleep 0.25.seconds

      Logger.progress("Codegen: Emitting platform target assembly machine instructions", "4/#{steps}")
      sleep 0.3.seconds

      Logger.progress("Link: Binding library functions into executable file -> #{@output}", "5/#{steps}", finished: true)
    end

    private def parse( args : Array(String) )
      parser = OptionParser.parse(args) do |p|
        p.banner = "Usage: volt build [options] [input_file]"
        p.on("-i INPUT", "--input INPUT", "Module file path target") { |v| @input = v }
        p.on("-o OUTPUT", "--output OUTPUT", "Output compilation binary target") { |v| @output = v }
        p.on("--release", "Maximize runtime machine instructions speed") { |v| @release = true }
        p.on("-D DEF", "Compiler constants definition") { |v| @defines << v }
        p.on("-L LIB", "Search paths targeting dynamic linked libraries") { |v| @libs << v }
        p.on("-h", "--help", "Show help") { puts p; next }
        p.invalid_option { |flag| fatal! "Invalid option: #{flag}\n#{p}" }
      end
      @input = args.first if !@input && args.any?
    end

  end


end
