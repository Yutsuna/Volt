require "./LineEditor"


module Volt::CLI


  class LineRenderer

    getter io : IO

    def initialize( @prompt : String, @io : IO = STDOUT )
    end

    def render( state : LineEditorState ) : Nil
      @io.print "\r"
      @io.print @prompt
      @io.print state.buffer.join
      @io.print "\e[K"

      # Cursor math is in chars, not display columns: wide/combining glyphs are out of scope.
      steps_left = state.buffer.size - state.cursor
      @io.print "\e[#{steps_left}D" if steps_left > 0
      @io.flush
    end

  end


end
