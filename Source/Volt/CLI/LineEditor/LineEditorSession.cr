require "./LineEditor"
require "./LineRenderer"
require "./InputEvent"
require "./Completion/CompletionEngine"


module Volt::CLI


  enum LineReadStatus
    Submitted
    Cancelled   # Ctrl+C — discard the line, nothing is evaluated
    Eof         # Ctrl+D — end the session
  end

  record LineReadResult, status : LineReadStatus, line : String = ""


  class LineEditorSession

    def initialize( @state : LineEditorState, @renderer : LineRenderer,
                    @completion : CompletionEngine? = nil )
    end

    # Blocking driver: yields for the next event, applies it, re-renders.
    def run( & : -> InputEvent ) : LineReadResult
      @renderer.render( @state )

      loop do
        event = yield
        result = handle( event )
        return result if result
        @renderer.render( @state )
      end
    end

    # Pure dispatch: applies one event, returns a result when the line terminates.
    def handle( event : InputEvent ) : LineReadResult?
      case event.key
      when KeyEvent::Enter
        @renderer.io.print "\r\n"
        return LineReadResult.new( LineReadStatus::Submitted, @state.text )
      when KeyEvent::CtrlC
        @renderer.io.print "^C\r\n"
        return LineReadResult.new( LineReadStatus::Cancelled )
      when KeyEvent::CtrlD
        return LineReadResult.new( LineReadStatus::Eof )
      when KeyEvent::Char       then @state.insert( event.char.not_nil! )
      when KeyEvent::Text       then @state.insert_text( event.text.not_nil! )
      when KeyEvent::Backspace  then @state.backspace
      when KeyEvent::Delete     then @state.delete
      when KeyEvent::ArrowLeft  then @state.move_left
      when KeyEvent::ArrowRight then @state.move_right
      when KeyEvent::CtrlLeft   then @state.move_word_left
      when KeyEvent::CtrlRight  then @state.move_word_right
      when KeyEvent::CtrlA, KeyEvent::Home then @state.move_to_start
      when KeyEvent::CtrlE, KeyEvent::End  then @state.move_to_end
      when KeyEvent::Tab        then complete
      when KeyEvent::ArrowUp
        @state.load_history( @state.history_index - 1 ) if @state.history_index > 0
      when KeyEvent::ArrowDown
        @state.load_history( @state.history_index + 1 ) if @state.history_index < @state.history.size
      when KeyEvent::Ignored
        # Recognized but deliberately inert (Insert, F-keys, unknown sequences).
      end

      nil
    end

    #------------------------------------------------------------------------------------

    private def complete : Nil
      engine = @completion
      return if engine.nil?

      result = engine.complete( @state.text, @state.cursor )
      return if result.nil?

      labels = result.candidates.map( &.label )

      if labels.size == 1
        @state.replace_range( result.replace_start, result.replace_end, labels.first )
        return
      end

      current = @state.text[ result.replace_start...result.replace_end ]
      prefix = common_prefix( labels )

      if prefix.size > current.size
        @state.replace_range( result.replace_start, result.replace_end, prefix )
      else
        list_candidates( labels )
      end
    end

    private def common_prefix( labels : Array(String) ) : String
      prefix = labels.first
      labels.each do |label|
        while !label.starts_with?( prefix )
          prefix = prefix[ 0, prefix.size - 1 ]
        end
      end
      prefix
    end

    private def list_candidates( labels : Array(String) ) : Nil
      io = @renderer.io
      io.print "\r\n"

      line_width = 0
      labels.each do |label|
        if line_width > 0 && line_width + label.size + 2 > 80
          io.print "\r\n"
          line_width = 0
        end
        io.print label
        io.print "  "
        line_width += label.size + 2
      end

      io.print "\r\n"
    end

  end


end
