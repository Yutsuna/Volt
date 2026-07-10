require "./ACommand"

module Volt::CLI

  class REPLCommand < ACommand

    register "repl", "Interactive Read-Eval-Print-Loop"

    @session : REPL::REPLSession = REPL::REPLSession.new
    @buffer : Array(String) = [] of String
    @cli_history : Array(String) = [] of String
    @loaded_files = [] of String

    @no_history : Bool = false
    @input : String? = nil
    @completion_engine : CompletionEngine? = nil

    include REPL::REPLBuiltins
    include REPL::REPLHistory

    #------------------------------------------------------------------------------------

    def execute( args : Array(String) )
      parse args
      prologue

      file_path = args.first? || @input
      dispatch_builtin( "load", file_path ) if file_path

      while session_should_continue?
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

    private def evaluate_file( path : String ) : Nil
      Logger.progress( "Loading #{path}...", finished: true )
      Fiber.yield

      if !File.exists?( path )
        Logger.error( "File not found: #{path}" )
        Fiber.yield
        return
      end

      if @loaded_files.includes? path
        Logger.warn( "File already loaded: #{path}" )
        Logger.info( "Use :reload to reload the file." )
        Fiber.yield
        return
      end

      begin
        source = File.read path
        result = @session.load_source( source, path, STDOUT, STDERR )

        if result.ok?
          @loaded_files << path unless @loaded_files.includes? path
          Logger.info( "Loaded.", "repl" )
          Fiber.yield
        else
          render_diagnostics( source, result.diagnostics, path )
        end
      rescue ex
        Logger.error( "Failed to evaluate file: #{ex.message}\n#{ex.backtrace.join("\n")}" )
        Fiber.yield
      end
    end

    #------------------------------------------------------------------------------------

    private def session_should_continue? : Bool
      result = read_next_line

      case result.status
      when LineReadStatus::Eof
        puts
        return false
      when LineReadStatus::Cancelled
        if !@buffer.empty?
          Logger.warn( "Multiline block discarded" )
          Fiber.yield
          @buffer.clear
        end
        return true
      end

      line = result.line
      trimmed = line.strip

      if trimmed.starts_with?(':')
        write_history( line )
        parts = trimmed.split(' ', limit: 2)
        cmd_name = parts[0][1..-1]
        arg = parts.size > 1 ? parts[1].strip : nil
        return dispatch_builtin( cmd_name, arg )
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

    private def read_next_line : LineReadResult
      prompt = get_prompt
      if STDIN.tty?
        read_line_interactive( prompt, @cli_history )
      else
        line = STDIN.gets
        line.nil? ? LineReadResult.new( LineReadStatus::Eof ) : LineReadResult.new( LineReadStatus::Submitted, line )
      end
    end

    private def evaluate( current_input : String ) : Nil
      result = @session.evaluate( current_input, STDOUT, STDERR )
      write_history( current_input )

      if result.ok?
        display_result result.value
      else
        render_diagnostics( current_input, result.diagnostics )
      end

    end

    private def display_result( val : IR::Value? ) : Nil
      return if val.nil? || val.is_nil?
      raw_display = if (vm = @session.vm)
        vm.inspect_value( val )
      else
        val.to_display
      end
      puts "=> #{REPL::REPLSyntaxHighlighter.highlight(raw_display)}"
    end

    private def render_diagnostics( source : String, diags : Frontend::DiagnosticBag?, file_label : String = "<repl>" ) : Nil
      if diags
        DiagnosticRenderer.new( { file_label => source } ).render( diags )
      end
    end

    #------------------------------------------------------------------------------------

    private def read_line_interactive( prompt : String, history : Array(String) ) : LineReadResult
      term = Terminal.new
      term.enable_raw_mode

      state = LineEditorState.new history
      renderer = LineRenderer.new prompt
      editor = LineEditorSession.new( state, renderer, completion_engine )

      begin
        editor.run { InputReader.read_event }
      ensure
        term.restore
      end
    end

    private def completion_engine : CompletionEngine
      @completion_engine ||= CompletionEngine.new( [
        BuiltinCommandProvider.new,
        SessionSymbolProvider.new( @session ),
        KeywordProvider.new,
      ] of ACompletionProvider )
    end

    #------------------------------------------------------------------------------------

    private def prologue : Nil
      Logger.info( "Volt REPL v#{Volt::VERSION}" )
      __builtin_help nil
      load_history
    end

    private def epilogue : Nil
      Logger.info( "Interactive session closed" )
      Fiber.yield
    end


  end

end
