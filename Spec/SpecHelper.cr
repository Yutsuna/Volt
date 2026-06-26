module Volt::Spec


  VOLT_BIN = File.join(__DIR__, "..", "bin", "Volt")


  module MStreamable


    macro included
      getter stdout : String
      getter stderr : String
      getter exit_code : Int32
    end


  end


  class RunVolt

    include MStreamable

    def initialize( file_path : String, command : String = "run" )
      out_io = IO::Memory.new
      err_io = IO::Memory.new

      status = Process.run(
        VOLT_BIN,
        args: [command, file_path],
        output: out_io,
        error: err_io
      )

      @stdout = out_io.to_s
      @stderr = err_io.to_s
      @exit_code = status.exit_code
    end

    def self.run( file_path : String ) : self
      new file_path
    end

  end


end
