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
        return EXIT_SUCCESS
      else
        return EXIT_ERROR
      end

      ensure
      Logger.stop
    end
  end


end
