require "./ACommand"

module Volt::CLI


  class Help < ACommand
    register "help", "Display the usage"

    def execute(args : Array(String))
      puts( "Volt Language" ).colorize(:yellow).bold
      puts( "Usage: volt <command> [options]" )
      max_len = Command.registry.keys.max_of(&.size)
      ACommand.registry.each do |name, cmd|
        printf "  %s  %s\n", name.ljust(max_len).colorize(:green), cmd.description
      end
    end
  end


end
