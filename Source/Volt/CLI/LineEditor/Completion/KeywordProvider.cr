require "./ACompletionProvider"


module Volt::CLI


  class KeywordProvider < ACompletionProvider

    def complete( context : CompletionContext ) : Array(CompletionCandidate)
      return [] of CompletionCandidate if context.word.starts_with?( ':' )

      Frontend::Lexer::KEYWORDS.keys
        .select( &.starts_with?( context.word ) )
        .map { |keyword| CompletionCandidate.new( keyword, :keyword ) }
    end

  end


end
