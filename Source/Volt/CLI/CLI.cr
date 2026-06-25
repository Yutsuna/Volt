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

      EXIT_ERROR

      ensure
      Logger.stop
    end
  end


end
