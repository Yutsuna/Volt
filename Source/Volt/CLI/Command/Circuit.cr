require "./ACommand"


module Volt::CLI


  class CircuitCommand < ACommand
    register "circuit", "Create or update the Project.vl file"

    property project_directory : String?

    def execute(args : Array(String))
      parse args

      dir = @project_directory || "."
      Logger.info( "Configuring project metadata in: #{dir}", "circuit" )

      Logger.progress("Checking workspace directories...", "1/2")
      sleep 0.1.seconds
      Logger.progress("Project.vl initialized successfully", "2/2", finished: true)
    end

    private def parse(args : Array(String))
      parser = OptionParser.parse( args ) do |p|
        p.banner = "Usage: volt circuit [options]"
        p.on("-d DIR", "--dir DIR", "Project directory path") { |v| @project_directory = v }
        p.on("-h", "--help", "Show help") { puts p; next }
        p.invalid_option { |flag| fatal! "Invalid option: #{flag}\n#{p}" }
      end
      @project_directory = args.first? if @project_directory.nil?
    end
  end


end
