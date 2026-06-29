require "../SpecHelper"


module Volt::Spec


  SAMPLES_01_DIR = File.join(__DIR__, "Samples/Tests/01.*.vl")


  class ExpectedResults

    include MStreamable

    EXPECTED_DATA = {
      "01.a.HelloWorld.vl"  =>  {exit_code: 0,  stderr: "", stdout: "Hello, Volt!\n"},
      "01.b.Arithmetic.vl"  =>  {exit_code: 0,  stderr: "", stdout: "PASS\n"*44},
      "01.c.Macro.vl"       =>  {exit_code: 0,  stderr: "", stdout: "[INFO] #{SAMPLES_01_DIR}/01.c.Macro.vl (line 20) : expression is: foo\n[INFO] #{SAMPLES_01_DIR}/01.c.Macro.vl (line 22) : expression is: bar\n"},
      "01.d.MacroControl.vl" => {exit_code: 0,  stderr: "", stdout: "LOW\nHIGH\na fruit\na vegetable\nunknown\n"},
      "01.e.MacroCodegen.vl" => {exit_code: 0,  stderr: "", stdout: "world\nvolt\nsection\ndone\n"},
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
