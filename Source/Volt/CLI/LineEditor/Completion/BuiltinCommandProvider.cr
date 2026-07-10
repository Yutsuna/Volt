require "./ACompletionProvider"


module Volt::CLI


  class BuiltinCommandProvider < ACompletionProvider

    def complete( context : CompletionContext ) : Array(CompletionCandidate)
      return [] of CompletionCandidate unless context.word.starts_with?( ':' )

      REPL::REPLBuiltins::BUILTINS.keys
        .map { |name| ":#{name}" }
        .select( &.starts_with?( context.word ) )
        .map { |label| CompletionCandidate.new( label, :builtin ) }
    end

  end


end
