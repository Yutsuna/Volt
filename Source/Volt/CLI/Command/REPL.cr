require "./ACommand"


module Volt::CLI


  class REPLCommand < ACommand
    register "repl", "Interactive Read-Eval-Print-Loop"

    @session : REPL::REPLSession
    @buffer : Array(String)

    HISTORY_FILE = ".volt_history"

    def initialize
      super
      @session = REPL::REPLSession.new
      @buffer = [] of String
    end

    def execute(args : Array(String))
      prologue

      loop do
        prompt = @buffer.empty? ? "volt> ".colorize(:cyan) : "volt* ".colorize(:dark_gray)
        print prompt
        STDOUT.flush

        line = STDIN.gets

        if line.nil?
          puts
          break
        end

        trimmed = line.strip

        if trimmed == "exit" && @buffer.empty?
          break
        elsif trimmed == "clear" && @buffer.empty?
          @session.clear
          @buffer.clear
          Logger.info( "Session history and compiled definitions cleared", "repl" )
          Fiber.yield
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
