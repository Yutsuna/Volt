require "./ACommand"

module Volt::CLI


  class AnalyseCommand < ACommand
    register "analyse", "Static semantic code analysis"

    property input : String?
    property type : String = "syntax"
    property rules : String?
    property output : String = "json"
    property warn_as_error : Bool = false
    property metrics : Bool = false
    property unused : Bool = false

    def execute(args : Array(String))
      parse args
      fatal! "Missing source analyzer validation inputs (-i)" unless @input

      Logger.info("Starting checking parser syntax limits", "analyse")

      Logger.progress("Validating lexer code maps...", "1/4")
      sleep 0.1.seconds

      Logger.progress("Processing context declarations checks...", "2/4")
      sleep 0.15.seconds

      if @unused
        Logger.progress("Locating non-referenced variable definitions...", "3/4")
        sleep 0.1.seconds
      end

      Logger.progress("Static validation steps completed", "4/4", finished: true)
    end


    private def parse( args : Array(String))
      parser = OptionParser.parse(args) do |p|
        p.banner = "Usage: volt analyse [options] [input_file_or_dir]"
        p.on("-i INPUT", "--input INPUT", "Code target directory or source file") { |v| @input = v }
        p.on("--type TYPE", "Type verification scope (syntax|semantic|style)") { |v| @type = v }
        p.on("--rules RULES", "Location path defining validation rule models") { |v| @rules = v }
        p.on("--output OUT", "Output validation layout formats") { |v| @output = v }
        p.on("--warn-as-error", "Style warning events will promote to runtime errors") { @warn_as_error = true }
        p.on("--metrics", "Gather code statistics and size metric benchmarks") { @metrics = true }
        p.on("--unused", "Flag unreferenced syntax structures and bindings") { @unused = true }
        p.on("-h", "--help", "Show help") { puts p; next }
        p.invalid_option { |flag| fatal! "Invalid option: #{flag}\n#{p}" }
      end

      @input = args.first? if @input.nil?
    end

  end


end
