require "../SpecHelper"


module Volt::Spec


  class ExpectedResults

    include MStreamable

    EXPECTED_DATA = {
      "01.a.HelloWorld.vl"  =>  {exit_code: 0,  stderr: "", stdout: "Hello, Volt!\n"},
      "01.b.Arithmetic.vl"  =>  {exit_code: 0,  stderr: "", stdout: "PASS\n"*44},
      "01.c.Macro.vl"       =>  {exit_code: 0,  stderr: "", stdout: "[INFO] /home/Yutsuna/Volt/Samples/Tests/01.c.Macro.vl (line 20) : expression is: foo\n[INFO] /home/Yutsuna/Volt/Samples/Tests/01.c.Macro.vl (line 22) : expression is: bar"},
    }

    def initialize( file_path : String )
      data = EXPECTED_DATA[ file_path.split("/").last ]
      @exit_code = data[:exit_code]
      @stderr    = data[:stderr]
      @stdout    = data[:stdout]
    end

    def self.run( file_path : String )
      new file_path
    end

  end


end
