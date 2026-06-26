module Volt::Frontend


  class CompilationError < Exception
    getter bag : DiagnosticBag

    def initialize( @bag : DiagnosticBag )
      super( "compilation failed with #{@bag.error_count} error(s)" )
    end
  end


end
