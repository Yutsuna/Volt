module Volt::CLI


  EXIT_SUCCESS = 0
  EXIT_ERROR = 84


  def self.run( args = ARGV ) : Int32
    Logger.start

    begin
      command_name = args.shift?
      command_class = ACommand.registry[ command_name ]?

      if command_class
        command_class.new.execute( args )
        EXIT_SUCCESS
      end

    rescue ex : SystemExit
      EXIT_ERROR

    rescue ex : Exception
      Logger.error( "#{ex.message}" )
      Logger.error( ex.backtrace.join( "\n" ) )
      EXIT_ERROR

    ensure
      Logger.stop
    end

    EXIT_ERROR
  end


end
