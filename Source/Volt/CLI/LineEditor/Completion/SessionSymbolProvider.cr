require "./ACompletionProvider"


module Volt::CLI


  class SessionSymbolProvider < ACompletionProvider

    def initialize( @session : REPL::REPLSession )
    end

    def complete( context : CompletionContext ) : Array(CompletionCandidate)
      candidates = [] of CompletionCandidate
      return candidates if context.word.starts_with?( ':' )

      prefix = context.word
      state = @session.state

      state.top_level_globals.each_key do |name|
        candidates << CompletionCandidate.new( name, :variable ) if name.starts_with?( prefix )
      end

      state.signatures.each do |name, sig|
        next unless sig.owner.nil?
        candidates << CompletionCandidate.new( name, :function ) if name.starts_with?( prefix )
      end

      state.types.each do |name, type_info|
        candidates << CompletionCandidate.new( name, :type ) if name.starts_with?( prefix )
        type_info.methods.each_key do |method_name|
          candidates << CompletionCandidate.new( method_name, :method ) if method_name.starts_with?( prefix )
        end
      end

      candidates
    end

  end


end
