module Volt
  module Driver


    # read -> lex -> parse -> sema -> codegen -> link.
    class FDriver

      getter reporter : Diagnostic::FReporter

      #--------------------------------------------------------------------------

      def initialize ( @source_path : String )
        @reporter = Diagnostic::FReporter.new(@source_path)
      end

      #--------------------------------------------------------------------------

      # Produce LLVM IR text, or nil if an earlier stage failed.
      def emit_ir : String?
        source  = File.read(@source_path)
        FLog.command "Lexing #{@source_path}..."
        tokens  = Lexer::FLexer.new(source, @reporter).scan
        FLog.command_done "\t#{tokens.size} tokens."
        return nil if @reporter.had_error?

        FLog.command "Parsing #{@source_path}..."
        program = Parser::FParser.new(tokens, @reporter).parse
        return nil if @reporter.had_error?
        FLog.command_done "\t#{program.top_level.size} top-level declarations."

        FLog.command "Semantic #{@source_path}..."
        Sema::FSema.new(program, @reporter).analyze
        FLog.command_done "\t#{program.top_level.size} top-level declarations."
        return nil if @reporter.had_error?

        ir = Codegen::FCodegen.new(program, @reporter).generate
        return nil if @reporter.had_error?

        ir
      rescue Diagnostic::CompileError
        nil
      end

      #--------------------------------------------------------------------------

      def compile ( output : String ) : Bool
        ir = emit_ir
        return false unless ir
        FLog.step "#{@source_path} -> #{output}: Linking #{ir.size} bytes of LLVM IR..."
        FLog.command "Linking #{@source_path}..."
        rv = Backend::FLinker.new(@reporter).build(ir, output)
        FLog.command_done "\t#{File.size(output)} bytes."
        rv
      end

      def run : Int32
        output = File.tempname("volt", "")
        return 1 unless compile(output)
        status = Process.run(output,
          input:  Process::Redirect::Inherit,
          output: Process::Redirect::Inherit,
          error:  Process::Redirect::Inherit)
        status.exit_code
      ensure
        File.delete?(output) if output
      end

    end


  end
end
