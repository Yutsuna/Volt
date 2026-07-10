require "./ACompletionProvider"


module Volt::CLI


  class CompletionEngine

    def initialize( @providers : Array(ACompletionProvider) )
    end

    def complete( text : String, cursor : Int32 ) : CompletionResult?
      context = build_context( text, cursor )
      return nil if context.word.empty?

      candidates = @providers.flat_map( &.complete( context ) )
      candidates = candidates.uniq( &.label ).sort_by( &.label )
      return nil if candidates.empty?

      CompletionResult.new( candidates, context.word_start, cursor )
    end

    private def build_context( text : String, cursor : Int32 ) : CompletionContext
      chars = text.chars
      start = cursor

      while start > 0 && word_char?( chars[ start - 1 ] )
        start -= 1
      end

      # A leading ':' at line start marks a builtin command word.
      if start > 0 && chars[ start - 1 ] == ':' && chars[ 0, start - 1 ].all?( &.whitespace? )
        start -= 1
      end

      word = chars[ start, cursor - start ].join
      CompletionContext.new( text, cursor, word, start )
    end

    private def word_char?( char : Char ) : Bool
      char.alphanumeric? || char == '_' || char == '?' || char == '!'
    end

  end


end
