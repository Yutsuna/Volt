require "./LineEditor"


module Volt::CLI


  class LineRenderer

    def initialize( @prompt : String )
    end

    def render( state : LineEditorState ) : Nil
      print "\r"
      print @prompt
      print state.buffer.join
      print "\e[K"

      steps_left = state.buffer.size - state.cursor
      print "\e[#{steps_left}D" if steps_left > 0
      STDOUT.flush
    end

  end


end
