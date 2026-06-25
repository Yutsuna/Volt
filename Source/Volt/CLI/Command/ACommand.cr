module Volt::CLI


  abstract class ACommand
    @@registry = {} of String => ACommand.class

    def self.registry
      @@registry
    end

    macro register(name, desc)
       def self.command_name : String
         {{name}}
       end

       def self.description : String
         {{desc}}
       end

       ::Volt::CLI::ACommand.registry[{{name}}] = self
     end

     abstract def execute(args : Array(String))
  end


end
