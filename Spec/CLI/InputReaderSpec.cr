require "spec"
require "../../Source/Volt/__all__"


module Volt::Spec

  private def self.event_for( bytes : String ) : Volt::CLI::InputEvent
    Volt::CLI::InputReader.reset
    Volt::CLI::InputReader.read_event( IO::Memory.new( bytes ) )
  end


  describe "Volt::CLI::InputReader" do

    it "maps Delete (ESC[3~)" do
      event_for( "\e[3~" ).key.should eq Volt::CLI::KeyEvent::Delete
    end

    it "maps Home variants" do
      event_for( "\e[H" ).key.should eq Volt::CLI::KeyEvent::Home
      event_for( "\e[1~" ).key.should eq Volt::CLI::KeyEvent::Home
      event_for( "\e[7~" ).key.should eq Volt::CLI::KeyEvent::Home
      event_for( "\eOH" ).key.should eq Volt::CLI::KeyEvent::Home
    end

    it "maps End variants" do
      event_for( "\e[F" ).key.should eq Volt::CLI::KeyEvent::End
      event_for( "\e[4~" ).key.should eq Volt::CLI::KeyEvent::End
      event_for( "\e[8~" ).key.should eq Volt::CLI::KeyEvent::End
      event_for( "\eOF" ).key.should eq Volt::CLI::KeyEvent::End
    end

    it "ignores Insert (ESC[2~)" do
      event_for( "\e[2~" ).key.should eq Volt::CLI::KeyEvent::Ignored
    end

    it "ignores unknown CSI sequences such as F5" do
      event_for( "\e[15~" ).key.should eq Volt::CLI::KeyEvent::Ignored
    end

    it "ignores a lone escape" do
      event_for( "\e" ).key.should eq Volt::CLI::KeyEvent::Ignored
    end

    it "maps arrows and Ctrl+arrows" do
      event_for( "\e[A" ).key.should eq Volt::CLI::KeyEvent::ArrowUp
      event_for( "\e[D" ).key.should eq Volt::CLI::KeyEvent::ArrowLeft
      event_for( "\e[1;5C" ).key.should eq Volt::CLI::KeyEvent::CtrlRight
      event_for( "\e[1;5D" ).key.should eq Volt::CLI::KeyEvent::CtrlLeft
    end

    it "maps Tab" do
      event_for( "\t" ).key.should eq Volt::CLI::KeyEvent::Tab
    end

    it "maps control keys" do
      event_for( "\u{1}" ).key.should eq Volt::CLI::KeyEvent::CtrlA
      event_for( "\u{3}" ).key.should eq Volt::CLI::KeyEvent::CtrlC
      event_for( "\u{4}" ).key.should eq Volt::CLI::KeyEvent::CtrlD
      event_for( "\u{5}" ).key.should eq Volt::CLI::KeyEvent::CtrlE
      event_for( "\r" ).key.should eq Volt::CLI::KeyEvent::Enter
      event_for( "\u{7f}" ).key.should eq Volt::CLI::KeyEvent::Backspace
    end

    it "decodes a multi-byte UTF-8 char as a single Char" do
      event = event_for( "é" )
      event.key.should eq Volt::CLI::KeyEvent::Char
      event.char.should eq 'é'
    end

    it "returns a pasted run as a single Text event" do
      event = event_for( "héllo" )
      event.key.should eq Volt::CLI::KeyEvent::Text
      event.text.should eq "héllo"
    end

    it "stops a text run at a control byte and keeps it queued" do
      Volt::CLI::InputReader.reset
      io = IO::Memory.new( "ab\rcd" )
      first = Volt::CLI::InputReader.read_event( io )
      first.key.should eq Volt::CLI::KeyEvent::Text
      first.text.should eq "ab"
      Volt::CLI::InputReader.read_event( io ).key.should eq Volt::CLI::KeyEvent::Enter
      second = Volt::CLI::InputReader.read_event( io )
      second.key.should eq Volt::CLI::KeyEvent::Text
      second.text.should eq "cd"
    end

    it "returns CtrlD on EOF" do
      event_for( "" ).key.should eq Volt::CLI::KeyEvent::CtrlD
    end

  end


end
