module Volt::CLI


  EXIT_SUCCESS =  0
  EXIT_ERROR   = 84

  def self.run(args = ARGV) : Int32
    Logger.start

    begin
      command_name = args.shift?
      unless command_name
        Logger.error("No command specified.")
        Logger.error("Available commands: #{ACommand.registry.keys.join(", ")}")
        return EXIT_ERROR
      end

      command_class = ACommand.registry[command_name]?
      unless command_class
        Logger.error("Unknown command: #{command_name}")
        Logger.error("Available commands: #{ACommand.registry.keys.join(", ")}")
        return EXIT_ERROR
      end

      command_class.new.execute(args)
      EXIT_SUCCESS

    rescue ex : SystemExit
      EXIT_ERROR

    rescue ex : RequestExit
      EXIT_SUCCESS

    rescue ex : Exception
      Logger.error(ex.message || "An unexpected error occurred")
      Logger.error(ex.backtrace.join("\n")) if ex.backtrace
      EXIT_ERROR

    ensure
      Logger.stop
    end
  end


end
