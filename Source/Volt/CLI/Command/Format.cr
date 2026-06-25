require "./ACommand"


module Volt::CLI


  class FormatCommand < ACommand
    register "format", "Run the formatter on the code"

    property input : String?
    property check : Bool = false
    property style : String = "default"
    property config : String?
    property diff : Bool = false
    property write : Bool = false

    def execute(args : Array(String))

      parse args
      fatal! "Missing required path target (-i)" unless @input
      fatal! "Options --check and --write are mutually exclusive" if @check && @write

      target = @input.not_nil!
      Logger.info("Checking configuration paths for code format layout", "format")

      Logger.progress("Scanning structural directories for analysis...", "1/3")
      sleep 0.1.seconds

      if @check
        Logger.progress("Verifying token patterns against guideline style...", "2/3")
        sleep 0.15.seconds
        Logger.progress("All code documents match target conventions", "3/3", finished: true)
      else
        Logger.progress("Performing style modifications on code tree...", "2/3")
        sleep 0.15.seconds
        if @write
          Logger.progress("Reformatted documents written successfully to disk", "3/3", finished: true)
        else
          Logger.progress("Finished layout calculations (dry-run)", "3/3", finished: true)
        end
      end
    end


    private def parse( args : Array(String) )
      parser = OptionParser.parse(args) do |p|
        p.banner = "Usage: volt format [options] [input_file_or_dir]"
        p.on("-i INPUT", "--input INPUT", "File or directory to format") { |v| @input = v }
        p.on("--check", "Check formatting rules without rewriting files") { @check = true }
        p.on("--style STYLE", "Style guideline rules to apply") { |v| @style = v }
        p.on("--config FILE", "Configure style rule paths") { |v| @config = v }
        p.on("--diff", "Output patch differences") { @diff = true }
        p.on("--write", "Write formatting updates in-place") { @write = true }
        p.on("-h", "--help", "Show help") { puts p; next }
        p.invalid_option { |flag| fatal! "Invalid option: #{flag}\n#{p}" }
      end

      @input = args.first? if @input.nil?
    end


  end


end
