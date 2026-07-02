require "../SpecHelper"


module Volt::Spec


  SAMPLES_01_DIR = File.expand_path("../../Samples/Tests/01.*.vl", __DIR__)


  class ExpectedResults

    include MStreamable

    EXPECTED_DATA = {
      "01.a.HelloWorld.vl"  =>  {exit_code: 0,  stderr: "", stdout: "Hello, Volt!\n"},
      "01.b.Arithmetic.vl"  =>  {exit_code: 0,  stderr: "", stdout: "PASS\n"*44},
      "01.c.Macro.vl"       =>  {exit_code: 0,  stderr: "", stdout: "[INFO] #{File.dirname(SAMPLES_01_DIR)}/01.c.Macro.vl (line 9) : expression is: foo\n[INFO] #{File.dirname(SAMPLES_01_DIR)}/01.c.Macro.vl (line 11) : expression is: bar\n"},
      "01.d.MacroControl.vl" => {exit_code: 0,  stderr: "", stdout: "LOW\nHIGH\na fruit\na vegetable\nunknown\n"},
      "01.e.MacroCodegen.vl" => {exit_code: 0,  stderr: "", stdout: "world\nvolt\nsection\ndone\n"},
      "01.f.StructNoMethods.vl" => {exit_code: 0,  stderr: "", stdout: "PASS\n"},
      "01.g.StructWithMethods.vl" => {exit_code: 0,  stderr: "", stdout: "PASS\n"},
      "01.h.ClassRaii.vl" => {exit_code: 0,  stderr: "", stdout: "ENTERING SCOPE\nCONNECTED DB 101\nINIT SESSION_MANAGER\nLEAVING SCOPE\nFINALIZE SESSION_MANAGER\nDISCONNECTED DB 101\nTEST COMPLETE\n"},
      "01.i.ClassInheritance.vl" => {exit_code: 0,  stderr: "", stdout: "WRITE TO USB [FlashDrive] (16384MB): kernel_backup.bin\nPRINT TO NETWORK [OfficeJet]: report.pdf\n"},
      "01.j.ClassMixin.vl" => {exit_code: 0,  stderr: "", stdout: "[LOG] Verifying credentials for admin\n[LOG] Executing 'rm -rf /tmp' under session SESS_999\n[LOG] Verifying credentials for guest\n[LOG] DENIED access to guest\n"},
      "01.l.ClassMixinAbstract.vl" => {exit_code: 0,  stderr: "", stdout: "Book: 'Design Patterns' by Gang of Four\nElectronic: VoltBook Pro (24 months warranty)\nBook Tax: 11.0\nLaptop Tax: 299.998\n"},
    }

    def initialize( file_path : String )
      data        = EXPECTED_DATA[ file_path.split("/").last ]
      @exit_code  = data[:exit_code]
      @stderr     = data[:stderr]
      @stdout     = data[:stdout]
    end

    def self.run( file_path : String )
      new file_path
    end

  end


end
