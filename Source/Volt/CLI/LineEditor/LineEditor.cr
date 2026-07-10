module Volt::CLI


  class LineEditorState

    getter buffer           : Array(Char)
    getter cursor           : Int32
    getter history          : Array(String)
    property history_index  : Int32

    def initialize( @history = [] of String )
      @buffer = [] of Char
      @cursor = 0
      @history_index = @history.size
    end

    def insert( char : Char )
      @buffer.insert( @cursor, char )
      @cursor += 1
    end

    def insert_text( text : String )
      text.each_char { |char| insert( char ) }
    end

    def backspace
      if @cursor > 0
        @cursor -= 1
        @buffer.delete_at( @cursor )
      end
    end

    def delete
      @buffer.delete_at( @cursor ) if @cursor < @buffer.size
    end

    def replace_range( from : Int32, to : Int32, text : String )
      return if from < 0 || to > @buffer.size || from > to
      @buffer = @buffer[ 0, from ] + text.chars + @buffer[ to.. ]
      @cursor = from + text.size
    end

    def text : String
      @buffer.join
    end

    def move_left
      @cursor = Math.max( 0, @cursor - 1 )
    end

    def move_right
      @cursor = Math.min( @buffer.size, @cursor + 1 )
    end

    def move_to_start
      @cursor = 0
    end

    def move_to_end
      @cursor = @buffer.size
    end

    def move_word_left
      return if @cursor == 0
      while @cursor > 0 && @buffer[ @cursor - 1 ].whitespace?;  @cursor -=1; end
      while @cursor > 0 && !@buffer[ @cursor - 1 ].whitespace?; @cursor -=1; end
    end

    def move_word_right
      return if @cursor == @buffer.size
      while @cursor < @buffer.size && @buffer[@cursor].whitespace?;   @cursor += 1; end
      while @cursor < @buffer.size && !@buffer[@cursor].whitespace?;  @cursor += 1; end
    end

    def load_history( index : Int32 )
      return if index < 0 || index > @history.size
      @history_index = index
      if index == @history.size
        @buffer.clear
      else
        @buffer = @history[ index ].chars
      end
      @cursor = @buffer.size
    end

  end


end
