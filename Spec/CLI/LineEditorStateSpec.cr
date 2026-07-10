require "spec"
require "../../Source/Volt/__all__"


module Volt::Spec


  describe "Volt::CLI::LineEditorState" do

    it "inserts characters at the cursor" do
      state = Volt::CLI::LineEditorState.new
      state.insert( 'a' )
      state.insert( 'c' )
      state.move_left
      state.insert( 'b' )
      state.text.should eq "abc"
      state.cursor.should eq 2
    end

    it "inserts multi-byte text as one char per glyph" do
      state = Volt::CLI::LineEditorState.new
      state.insert_text( "héllo" )
      state.buffer.size.should eq 5
      state.text.should eq "héllo"
    end

    it "backspaces at start without effect" do
      state = Volt::CLI::LineEditorState.new
      state.backspace
      state.text.should eq ""
      state.cursor.should eq 0
    end

    it "forward-deletes the char under the cursor" do
      state = Volt::CLI::LineEditorState.new
      state.insert_text( "abcdef" )
      state.move_left
      state.move_left
      state.delete
      state.text.should eq "abcdf"
      state.cursor.should eq 4
    end

    it "forward-delete at end of line has no effect" do
      state = Volt::CLI::LineEditorState.new
      state.insert_text( "abc" )
      state.delete
      state.text.should eq "abc"
    end

    it "moves to start and end" do
      state = Volt::CLI::LineEditorState.new
      state.insert_text( "abc" )
      state.move_to_start
      state.cursor.should eq 0
      state.move_to_end
      state.cursor.should eq 3
    end

    it "replaces a range and places the cursor after it" do
      state = Volt::CLI::LineEditorState.new
      state.insert_text( "say wh now" )
      state.replace_range( 4, 6, "while" )
      state.text.should eq "say while now"
      state.cursor.should eq 9
    end

    it "moves by words" do
      state = Volt::CLI::LineEditorState.new
      state.insert_text( "foo bar" )
      state.move_word_left
      state.cursor.should eq 4
      state.move_word_left
      state.cursor.should eq 0
      state.move_word_right
      state.cursor.should eq 3
    end

  end


end
