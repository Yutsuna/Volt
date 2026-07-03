require "spec"
require "../SpecHelper"
require "../../Source/Volt/CLI/CLI"


module Volt::Spec


  describe "Volt::Run: 01" do
    Dir.glob( SAMPLES_01_DIR, "vl" ).each do |file|
      filename = File.basename file

      it "Should run without errors: #{filename}" do
        volt     = Volt::Spec::RunVolt.run file
        expected = Volt::Spec::ExpectedResults.run( file )

        if volt.exit_code == Volt::CLI::EXIT_ERROR
          log_build_failed( volt, filename )
        end

        assert_volt( volt.exit_code, expected.exit_code, "exit_code", filename )
        assert_volt( volt.stderr,    expected.stderr,    "stderr",    filename )
        assert_volt( volt.stdout,    expected.stdout,    "stdout",    filename )
      end
    end
  end


  private def self.assert_volt( actual : String | Int32, exp : String | Int32, type : String, filename : String )
    err : String = <<-ERR
    Mismatch in #{ type } for #{ filename }
      Expected: #{ exp.inspect }
      Got:      #{ actual.inspect }
    ERR
    actual.should( eq( exp ), err )
  end


  private def self.log_build_failed( volt : Volt::Spec::RunVolt, filename : String )
    puts "___DEBUG___ #{filename}"
    puts volt.stdout
    puts volt.stderr
    puts volt.exit_code
    puts "___END_DEBUG___ #{filename}"
  end


end
