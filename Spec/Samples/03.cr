require "spec"
require "../SpecHelper"
require "../../Source/Volt/CLI/CLI"


module Volt::Spec


  CIRCUITS_DIR = File.expand_path("../../Samples/Tests/Circuits", __DIR__)

  CIRCUIT_EXPECTED_DATA = {
    "TwoDeps"     => {exit_code: 0, stderr: "", stdout: "[user]\nYutsuna\n"},
    "DiamandDeps" => {exit_code: 0, stderr: "", stdout: ""},
  }


  describe "Volt::Run: 03 (Circuits)" do
    CIRCUIT_EXPECTED_DATA.each do |project, expected|
      it "Should run project-aware without errors: #{project}" do
        dir  = File.join( CIRCUITS_DIR, project )
        volt = Volt::Spec::RunVolt.run dir

        if volt.exit_code == Volt::CLI::EXIT_ERROR
          puts "___DEBUG___ #{project}"
          puts volt.stdout
          puts volt.stderr
          puts volt.exit_code
          puts "___END_DEBUG___ #{project}"
        end

        volt.exit_code.should eq( expected[:exit_code] )
        volt.stderr.should    eq( expected[:stderr] )
        volt.stdout.should    eq( expected[:stdout] )
      end
    end
  end


end
