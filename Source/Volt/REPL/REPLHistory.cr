require "../CLI/Logger"


module Volt::REPL


  module REPLHistory


    HISTORY_FILE = ".volt_history"


    private def load_history : Nil
      return if @no_history
      return unless File.exists?( HISTORY_FILE )

      File.each_line( HISTORY_FILE ) { |line| @cli_history << line unless line.blank? }
    rescue ex
      CLI::Logger.warn( "Failed to load history file: #{ex.message}", "repl" )
      Fiber.yield
    end


    private def write_history( source : String ) : Nil
      return if @no_history || source.blank?

      @cli_history << source

      File.open( HISTORY_FILE, "a" ) { |stream| stream.puts source }
    rescue ex
      CLI::Logger.warn( "Failed to append to history file: #{ex.message}", "repl" )
      Fiber.yield
    end

  end


end
