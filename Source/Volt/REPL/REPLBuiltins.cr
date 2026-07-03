require "../CLI/Logger"


module Volt::REPL


  module REPLBuiltins

    #------------------------------------------------------------------------------------

    BUILTINS = {} of String => NamedTuple(
      description: String,
      action: Proc(REPLBuiltins, String?, Bool)
    )

    #------------------------------------------------------------------------------------

    macro builtin(name, description, &block)
      {% str = name.stringify %}

      protected def __builtin_{{name.id}}( arg : String? ) : Bool
        {{ block.body }}
      end

      BUILTINS[ {{str}} ] = {
        description: {{description}},
        action: ->( receiver : REPLBuiltins, arg : String? ) {
          receiver.__builtin_{{name.id}}( arg )
        }
      }
    end

    #------------------------------------------------------------------------------------

    builtin exit, "Exit the REPL" do |arg|
      false
    end

    builtin quit, "Exit the REPL" do |arg|
      false
    end

    builtin clear, "Clear the session history and compiled definitions" do |arg|
      @session.clear
      @buffer.clear
      @cli_history.clear
      @loaded_files.clear
      print "\e[H\e[2J\e[3J"
      CLI::Logger.info( "Session history and compiled definitions cleared" )
      Fiber.yield
      true
    end

    builtin help, "Display available commands" do |arg|
      CLI::Logger.info( "Available commands:" )
      BUILTINS.each do |cmd, data|
        CLI::Logger.info( "  :#{cmd.ljust( 8 )} - #{data[:description]}" )
      end
      Fiber.yield
      true
    end

    builtin load, "Load definitions and run top-level from a file: :load <file>" do |arg|
      if arg.nil? || arg.empty?
        CLI::Logger.warn( "Usage: :load <file_path>", "repl" )
        Fiber.yield
        true
      else
        evaluate_file( arg )
        true
      end
    end

    builtin reload, "Reload all currently loaded files lazily" do |arg|
      if @loaded_files.empty?
        CLI::Logger.error( "No files loaded in the current session.", "repl" )
        Fiber.yield
      else
        @loaded_files.dup.each do |path|
          evaluate_file( path )
        end
      end
      true
    end

    builtin loaded, "Display all loaded files in the current session" do |arg|
      if @loaded_files.empty?
        CLI::Logger.info( "No files loaded in the current session.", "repl" )
      else
        CLI::Logger.info( "Loaded files:", "repl" )
        @loaded_files.each do |file|
          CLI::Logger.info( "  - #{file}", "repl" )
        end
      end
      Fiber.yield
      true
    end

    #------------------------------------------------------------------------------------

    private def dispatch_builtin( command_name : String, arg : String? ) : Bool
      if cmd = BUILTINS[ command_name ]?
        return cmd[:action].call( self, arg )
      end

      CLI::Logger.warn(
        "Unknown command: #{command_name}. Type :help to list available commands.",
        "repl"
      )
      Fiber.yield
      true
    end

  end


end
