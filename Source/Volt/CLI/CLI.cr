module Volt::CLI


  EXIt_SUCCESS = 0
  EXIT_ERROR = 84


  def self.run( args = ARGV ) : NoReturn
    command_name = args.shift
    command_class = ACommand.registry[ command_name ]?

    if command_class
      command_class.new.execute( args )
      exit EXIt_SUCCESS
    else
      exit EXIT_ERROR
    end
  end


end
