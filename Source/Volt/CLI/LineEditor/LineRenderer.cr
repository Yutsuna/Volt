require "./LineEditor"
require "./PosixTermios"
require "./Completion/CompletionMenu"


module Volt::CLI


  class LineRenderer

    ANSI_SEQUENCE = /\e\[[0-9;?]*[A-Za-z]/

    getter io : IO

    @prompt_width : Int32

    # `highlighter` recolors the input line on every repaint (live syntax
    # highlighting); it must only add ANSI codes, never change visible chars.
    def initialize( @prompt : String, @io : IO = STDOUT,
                    @highlighter : Proc(String, String)? = nil )
      @prompt_width = @prompt.gsub( ANSI_SEQUENCE, "" ).size
    end

    # Repaints the input line (prompt + highlighted buffer), then the completion
    # menu below it when open, and finally parks the cursor at the logical
    # position. `\e[J` wipes everything below the input line so a shrinking or
    # closing menu never leaves residue.
    def render( state : LineEditorState, menu : CompletionMenu? = nil ) : Nil
      text = state.text

      @io.print "\r"
      @io.print @prompt
      if highlighter = @highlighter
        @io.print highlighter.call( text )
      else
        @io.print text
      end
      @io.print "\e[J"

      if menu && menu.open?
        @io.print "\r\n"
        height = menu.display( @io, width: Terminal.width )
        @io.print "\e[#{height + 1}A" if height > 0
      end

      # Cursor math is in chars, not display columns: wide/combining glyphs are out of scope.
      column = @prompt_width + state.cursor
      @io.print "\r"
      @io.print "\e[#{column}C" if column > 0
      @io.flush
    end

  end


end
