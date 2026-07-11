require "./LineEditor"
require "./LineRenderer"
require "./InputEvent"
require "./Completion/CompletionEngine"
require "./Completion/CompletionMenu"


module Volt::CLI


  enum LineReadStatus
    Submitted
    Cancelled   # Ctrl+C — discard the line, nothing is evaluated
    Eof         # Ctrl+D — end the session
  end

  record LineReadResult, status : LineReadStatus, line : String = ""


  class LineEditorSession

    getter menu : CompletionMenu

    def initialize( @state : LineEditorState, @renderer : LineRenderer,
                    @completion : CompletionEngine? = nil )
      @menu = CompletionMenu.new
    end

    # Blocking driver: yields for the next event, applies it, re-renders.
    def run( & : -> InputEvent ) : LineReadResult
      @renderer.render( @state, @menu )

      loop do
        event = yield
        result = handle( event )
        return result if result
        @renderer.render( @state, @menu )
      end
    end

    # Pure dispatch: applies one event, returns a result when the line terminates.
    def handle( event : InputEvent ) : LineReadResult?
      case event.key
      when KeyEvent::Enter
        close_menu
        @renderer.io.print "\r\n"
        return LineReadResult.new( LineReadStatus::Submitted, @state.text )
      when KeyEvent::CtrlC
        close_menu
        @renderer.io.print "^C\r\n"
        return LineReadResult.new( LineReadStatus::Cancelled )
      when KeyEvent::CtrlD
        close_menu
        return LineReadResult.new( LineReadStatus::Eof )
      when KeyEvent::Char       then edit { @state.insert( event.char.not_nil! ) }
      when KeyEvent::Text       then edit { @state.insert_text( event.text.not_nil! ) }
      when KeyEvent::Backspace  then edit { @state.backspace }
      when KeyEvent::Delete     then edit { @state.delete }
      when KeyEvent::CtrlBackspace then edit { @state.delete_word_left }
      when KeyEvent::CtrlDelete    then edit { @state.delete_word_right }
      when KeyEvent::Tab        then complete_next
      when KeyEvent::ShiftTab   then complete_previous
      when KeyEvent::Escape     then @menu.close
      when KeyEvent::ArrowLeft  then move { @state.move_left }
      when KeyEvent::ArrowRight then move { @state.move_right }
      when KeyEvent::CtrlLeft   then move { @state.move_word_left }
      when KeyEvent::CtrlRight  then move { @state.move_word_right }
      when KeyEvent::CtrlA, KeyEvent::Home then move { @state.move_to_start }
      when KeyEvent::CtrlE, KeyEvent::End  then move { @state.move_to_end }
      when KeyEvent::ArrowUp
        @menu.close
        @state.load_history( @state.history_index - 1 ) if @state.history_index > 0
      when KeyEvent::ArrowDown
        @menu.close
        @state.load_history( @state.history_index + 1 ) if @state.history_index < @state.history.size
      when KeyEvent::Ignored
        # Recognized but deliberately inert (Insert, F-keys, unknown sequences).
      end

      nil
    end

    #------------------------------------------------------------------------------------

    # Text edits keep the menu open and re-filter it against the new word;
    # cursor moves dismiss it.
    private def edit( & ) : Nil
      yield
      refilter_menu
    end

    private def move( & ) : Nil
      @menu.close
      yield
    end

    # First Tab: query the engine; a single candidate completes directly, several
    # open the menu and insert their common root. Subsequent Tabs walk the menu.
    private def complete_next : Nil
      if @menu.open?
        apply_selection( @menu.selection_next )
        return
      end

      engine = @completion
      return if engine.nil?

      result = engine.complete( @state.text, @state.cursor )
      return if result.nil?

      if result.candidates.size == 1
        @state.replace_range( result.replace_start, result.replace_end, result.candidates.first.label )
        return
      end

      word = @state.text[ result.replace_start...result.replace_end ]
      @menu.show( result.candidates.map( &.label ), word, result.replace_start )

      root = @menu.common_root
      if root.size > word.size
        @state.replace_range( result.replace_start, result.replace_end, root )
        @menu.name_filter = root
      end
    end

    private def complete_previous : Nil
      return unless @menu.open?
      apply_selection( @menu.selection_previous )
    end

    private def apply_selection( label : String? ) : Nil
      return if label.nil?
      @state.replace_range( @menu.replace_start, @state.cursor, label )
    end

    # After an edit while the menu is open, the word under completion spans
    # [replace_start, cursor): re-filter on it, closing when the edit left the
    # completion span or nothing matches anymore.
    private def refilter_menu : Nil
      return unless @menu.open?

      if @state.cursor < @menu.replace_start
        @menu.close
        return
      end

      @menu.name_filter = @state.text[ @menu.replace_start...@state.cursor ]
    end

    # Closing on line termination repaints without the panel so no menu rows are
    # left behind under the submitted line.
    private def close_menu : Nil
      return unless @menu.open?
      @menu.close
      @renderer.render( @state, @menu )
    end

  end


end
