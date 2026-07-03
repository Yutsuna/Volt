require "./ACommand"

module Volt::CLI


  class REPLCommand < ACommand

    register "repl", "Interactive Read-Eval-Print-Loop"

    @session : REPL::REPLSession = REPL::REPLSession.new
    @buffer : Array(String) = [] of String
    @cli_history : Array(String) = [] of String

    @no_history : Bool = false
    @input : String? = nil

    HISTORY_FILE = ".volt_history"
    BUILTINS_COMMANDS_DISPATCH = {
      "exit"  => {symbol: :break, description: "Exit the REPL"},
      "clear" => {symbol: :clear, description: "Clear the session history and compiled definitions"},
      "help"  => {symbol: :help, description: "Display available commands"},
    } of String => NamedTuple(symbol: Symbol, description: String)

    #------------------------------------------------------------------------------------

    def execute( args : Array(String) )
      parse args
      prologue

      if file_path = @input
        preload file_path
      end

      while process_session_step
      end

      epilogue
    end

    #------------------------------------------------------------------------------------

    private def parse( args : Array(String) ) : Nil
      OptionParser.parse( args ) do |p|
        p.banner = "Usage: volt repl [options] [file]"
        p.on( "-i INPUT", "--input INPUT", "Pre-load a file into the REPL session" ) { |v| @input = v }
        p.on( "-n", "--no-history", "Disable history saving" ) { @no_history = true }
        p.on( "-h", "--help", "Show help" ) { puts p; raise RequestExit.new }
        p.invalid_option { |flag| fatal! "Invalid option: #{flag}\n#{p}" }
      end
      @input = args.first? if @input.nil?
    end

    #------------------------------------------------------------------------------------

    private def preload( file_path : String ) : Nil
      unless File.exists? file_path
        Logger.warn( "File not found: #{file_path}" )
        Fiber.yield
        return
      end

      Logger.progress( "Preloading #{file_path}...", finished: true )
      Fiber.yield

      begin
        content = File.read file_path
        result = @session.evaluate( content, STDOUT, STDERR )

        if result.ok?
          display_result result.value
        else
          render_diagnostics( content, result.diagnostics, file_path )
        end
      rescue ex
        Logger.error( "Failed to evaluate file: #{ex.message}" )
        Fiber.yield
      end
    end

    #------------------------------------------------------------------------------------

    private def process_session_step : Bool
      line = read_next_line
      if line.nil?
        puts
        return false
      end

      trimmed = line.strip

      case dispatch_command trimmed
      when :break
        return false
      when :next
        return true
      end

      if trimmed.empty?
        if !@buffer.empty?
          Logger.warn( "Multiline block discarded" )
          Fiber.yield
          @buffer.clear
        end
        return true
      end

      @buffer << line
      current_input = @buffer.join( "\n" )

      return true if REPL::REPLLineGuard.incomplete?( current_input )

      evaluate( current_input )
      @buffer.clear
      true
    end

    private def get_prompt : String
      @buffer.empty? ? "volt> ".colorize( :cyan ).to_s : "volt* ".colorize( :dark_gray ).to_s
    end

    private def read_next_line : String?
      prompt = get_prompt
      STDIN.tty? ? read_line_interactive( prompt, @cli_history ) : STDIN.gets
    end

    private def evaluate( current_input : String ) : Nil
      result = @session.evaluate( current_input, STDOUT, STDERR )

      unless @no_history
        write_history( current_input )
        @cli_history << current_input
      end

      if result.ok?
        display_result result.value
      else
        render_diagnostics( current_input, result.diagnostics )
      end
    end

    private def display_result( val : IR::Value? ) : Nil
      return if val.nil? || val.is_nil?
      raw_display = val.to_display
      puts "=> #{REPL::REPLSyntaxHighlighter.highlight(raw_display)}"
    end

    private def render_diagnostics( source : String, diags : Frontend::DiagnosticBag?, file_label : String = "<repl>" ) : Nil
      if diags
        DiagnosticRenderer.new( { file_label => source } ).render( diags )
      end
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
      @cli_history.clear
      print "\e[H\e[2J\e[3J"
      Logger.info( "Session history and compiled definitions cleared" )
      Fiber.yield
      :next
    end

    private def help : Symbol
      Logger.info( "Available commands:" )
      BUILTINS_COMMANDS_DISPATCH.each { |cmd, data| Logger.info( "  #{cmd} - #{data.description}" ) }
      Fiber.yield
      :next
    end

    #------------------------------------------------------------------------------------

    private def prologue : Nil
      Logger.info( "Volt REPL v#{Volt::VERSION}" )
      Logger.info( "Commands: exit (to quit), clear (to clear history), help (for info)" )
      Fiber.yield
      load_history @session unless @no_history
    end

    private def epilogue : Nil
      Logger.info( "Interactive session closed" )
      Fiber.yield
    end

    private def load_history( session : REPL::REPLSession ) : Nil
      return unless File.exists? HISTORY_FILE
      File.each_line( HISTORY_FILE ) { |line| @cli_history << line unless line.blank? }
    rescue ex
      Logger.warn( "Failed to load history file: #{ex.message}" )
      Fiber.yield
    end

    private def write_history( source : String ) : Nil
      File.open( HISTORY_FILE, "a" ) { |stream| stream.puts source }
    rescue ex
      Logger.warn( "Failed to append to history file: #{ex.message}" )
      Fiber.yield
    end

  end

end
