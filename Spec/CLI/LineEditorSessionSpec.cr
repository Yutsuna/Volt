require "spec"
require "../../Source/Volt/__all__"


module Volt::Spec


  class StubProvider < Volt::CLI::ACompletionProvider
    def initialize( @labels : Array(String) )
    end

    def complete( context : Volt::CLI::CompletionContext ) : Array(Volt::CLI::CompletionCandidate)
      @labels
        .select( &.starts_with?( context.word ) )
        .map { |label| Volt::CLI::CompletionCandidate.new( label, :stub ) }
    end
  end

  private def self.make_session( completion : Volt::CLI::CompletionEngine? = nil )
    state = Volt::CLI::LineEditorState.new
    renderer = Volt::CLI::LineRenderer.new( "> ", IO::Memory.new )
    { Volt::CLI::LineEditorSession.new( state, renderer, completion ), state }
  end

  private def self.char_event( char : Char )
    Volt::CLI::InputEvent.new( Volt::CLI::KeyEvent::Char, char )
  end

  private def self.key_event( key : Volt::CLI::KeyEvent )
    Volt::CLI::InputEvent.new( key )
  end


  describe "Volt::CLI::LineEditorSession" do

    it "submits the typed line on Enter, including multi-byte chars" do
      session, state = make_session
      "éx".each_char { |c| session.handle( char_event( c ) ).should be_nil }
      result = session.handle( key_event( Volt::CLI::KeyEvent::Enter ) )
      result.not_nil!.status.should eq Volt::CLI::LineReadStatus::Submitted
      result.not_nil!.line.should eq "éx"
    end

    it "cancels on Ctrl+C without submitting the buffer" do
      session, state = make_session
      session.handle( char_event( 'a' ) )
      result = session.handle( key_event( Volt::CLI::KeyEvent::CtrlC ) )
      result.not_nil!.status.should eq Volt::CLI::LineReadStatus::Cancelled
      result.not_nil!.line.should eq ""
    end

    it "returns Eof on Ctrl+D" do
      session, state = make_session
      result = session.handle( key_event( Volt::CLI::KeyEvent::CtrlD ) )
      result.not_nil!.status.should eq Volt::CLI::LineReadStatus::Eof
    end

    it "handles Delete, Home and End" do
      session, state = make_session
      "abcd".each_char { |c| session.handle( char_event( c ) ) }
      session.handle( key_event( Volt::CLI::KeyEvent::Home ) )
      state.cursor.should eq 0
      session.handle( key_event( Volt::CLI::KeyEvent::Delete ) )
      state.text.should eq "bcd"
      session.handle( key_event( Volt::CLI::KeyEvent::End ) )
      state.cursor.should eq 3
    end

    it "treats Ignored events as no-ops" do
      session, state = make_session
      session.handle( char_event( 'a' ) )
      session.handle( key_event( Volt::CLI::KeyEvent::Ignored ) )
      state.text.should eq "a"
      state.cursor.should eq 1
    end

    it "inserts Text events at the cursor" do
      session, state = make_session
      session.handle( Volt::CLI::InputEvent.new( Volt::CLI::KeyEvent::Text, text: "héllo" ) )
      state.text.should eq "héllo"
    end

    it "completes a single match on Tab" do
      engine = Volt::CLI::CompletionEngine.new( [ StubProvider.new( [ "foobar" ] ) ] of Volt::CLI::ACompletionProvider )
      session, state = make_session( engine )
      "fo".each_char { |c| session.handle( char_event( c ) ) }
      session.handle( key_event( Volt::CLI::KeyEvent::Tab ) )
      state.text.should eq "foobar"
      state.cursor.should eq 6
    end

    it "extends to the common prefix on Tab with multiple matches" do
      engine = Volt::CLI::CompletionEngine.new( [ StubProvider.new( [ "foobar", "foobaz" ] ) ] of Volt::CLI::ACompletionProvider )
      session, state = make_session( engine )
      "fo".each_char { |c| session.handle( char_event( c ) ) }
      session.handle( key_event( Volt::CLI::KeyEvent::Tab ) )
      state.text.should eq "fooba"
    end

  end


end
