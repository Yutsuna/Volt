module SpecHelper

  extend self

  ROOT = File.expand_path("../../..", __FILE__)

  #--------------------------------------------------------------------------

  def example_path ( name : String ) : String
    File.join(ROOT, "Examples", "Phase1", name)
  end

  def compile_and_run ( source_path : String ) : {Int32, String}
    output = File.tempname("voltspec", "")
    driver = Volt::Driver::FDriver.new(source_path)
    raise "compilation failed for #{source_path}" unless driver.compile(output)

    captured = IO::Memory.new
    status = Process.run(output, output: captured, error: Process::Redirect::Inherit)
    {status.exit_code, captured.to_s}
  ensure
    if output
      File.delete?(output)
      File.delete?("#{output}.ll")
    end
  end

  def run_example ( name : String ) : {Int32, String}
    compile_and_run(example_path(name))
  end

  def lex ( source : String ) : Array(Volt::Lexer::Token)
    reporter = Volt::Diagnostic::FReporter.new("<spec>")
    Volt::Lexer::FLexer.new(source, reporter).scan
  end

end
