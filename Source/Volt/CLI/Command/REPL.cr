require "./ACommand"


module Volt::CLI


  class REPLCommand < ACommand

    register "repl", "Interactive Read-Eval-Print-Loop"
    @session : REPL::REPLSession = REPL::REPLSession.new
    @buffer : Array(String) = [] of String

    HISTORY_FILE = ".volt_history"
    BUILTINS_COMMANDS_DISPATCH = {
      "exit"  => {symbol: :break, description: "Exit the REPL"},
      "clear" => {symbol: :clear, description: "Clear the session history and compiled definitions"},
      "help"  => {symbol: :help, description: "Display available commands"},
    } of String => NamedTuple(symbol: Symbol, description: String)

    #------------------------------------------------------------------------------------

    def execute(args : Array(String))
      prologue

      loop do
        prompt = @buffer.empty? ? "volt> ".colorize(:cyan) : "volt* ".colorize(:dark_gray)
        line = STDIN.tty? ? read_line_interactive( prompt.to_s, @session.history ) : STDIN.gets

        if line.nil?
          puts
          break
        end

        trimmed = line.strip

        case dispatch_command( trimmed )
        when :break
          break
        when :next
          next
        end

        if trimmed.empty? && !@buffer.empty?
          Logger.warn( "Multiline block discarded", "repl" )
          Fiber.yield
          @buffer.clear
          next
        end

        next if trimmed.empty?

        @buffer << line
        current_input = @buffer.join "\n"

        next if REPL::REPLLineGuard.incomplete? current_input

        result = @session.evaluate( current_input, STDOUT, STDERR )

        if result.ok?
          val = result.value
          unless val.nil? || val.is_nil?
            raw_display = val.to_display
            puts "=> #{REPL::REPLSyntaxHighlighter.highlight(raw_display)}"
          end

          write_history current_input if result.definition_saved?
        else
          if diags = result.diagnostics
            DiagnosticRenderer.new( { "<repl>" => current_input } ).render diags
          end
        end

        @buffer.clear
      end

      epilogue
    end


    #------------------------------------------------------------------------------------

    private def read_line_interactive( prompt : String, history : Array(String) ) : String?
      term = Terminal.new
      term.enable_raw_mode

      state = LineEditorState.new history
      renderer = LineRenderer.new prompt

      renderer.render state

      begin
        loop do
          event = InputReader.read_char

          case event.key
          when KeyEvent::Enter
            puts
            return state.buffer.join
          when KeyEvent::Char
            state.insert( event.char.not_nil! )
          when KeyEvent::Backspace
            state.backspace
          when KeyEvent::ArrowLeft
            state.move_left
          when KeyEvent::ArrowRight
            state.move_right
          when KeyEvent::CtrlLeft
            state.move_word_left
          when KeyEvent::CtrlRight
            state.move_word_right
          when KeyEvent::CtrlA
            state.move_to_start
          when KeyEvent::CtrlC
            puts
            return state.buffer.join
          when KeyEvent::CtrlD
            return nil
          when KeyEvent::ArrowUp
            if state.history_index > 0
              state.load_history( state.history_index - 1 )
            end
          when KeyEvent::ArrowDown
            if state.history_index < history.size
              state.load_history( state.history_index + 1 )
            end
          end

          renderer.render( state )
        end
      ensure
        term.restore
      end
    end

    #------------------------------------------------------------------------------------

    private def dispatch_command( trimmed : String ) : Symbol?
      return nil unless @buffer.empty?
      action = BUILTINS_COMMANDS_DISPATCH[trimmed]?
      return nil unless action
      action[ :symbol ]
    end

    private def clear : Symbol
      @session.clear
      @buffer.clear
      print "\e[H\e[2J\e[3J"
      Logger.info( "Session history and compiled definitions cleared", "repl" )
      Fiber.yield
      :next
    end

    private def help : Symbol
      Logger.info( "Available commands:", "repl" )
      BUILTINS_COMMANDS_DISPATCH.each { |cmd, data| Logger.info( "  #{cmd} - #{data.description}", "repl" ) }
      Fiber.yield
      :next
    end

    #------------------------------------------------------------------------------------

    private def prologue : Nil
      Logger.info( "Volt Interactive Loop", "repl" )
      Logger.info( "Commands: exit (to quit), clear (to clear history)", "repl" )
      load_history @session
      Fiber.yield
    end

    private def epilogue : Nil
      Logger.info( "Interactive session closed", "repl" )
      Fiber.yield
    end

    private def load_history(session : REPL::REPLSession) : Nil
      return unless File.exists? HISTORY_FILE
      File.each_line( HISTORY_FILE ) { |line| session.history << line unless line.blank? }
    rescue ex
      Logger.warn( "Failed to load history file: #{ex.message}", "repl" )
      Fiber.yield
    end

    private def write_history(source : String) : Nil
      File.open( HISTORY_FILE, "a" ) { |stream| stream.puts source }
    rescue ex
      Logger.warn("Failed to append to history file: #{ex.message}", "repl")
      Fiber.yield
    end

  end


end
