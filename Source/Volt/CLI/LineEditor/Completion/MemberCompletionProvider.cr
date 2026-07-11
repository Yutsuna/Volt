require "./ACompletionProvider"


module Volt::CLI


  # Completes `receiver.<partial>` by resolving the receiver's static type from
  # the REPL's incremental state and listing that type's methods. No VM/runtime
  # lookup needed : `IncrementalState#types` already holds every Core (and
  # user-defined) type's method table once the session has evaluated anything.
  #
  # The lookup mirrors the interpreter's resolution chain: the exact type first
  # (`Int32`), then its primitive family reopenings (`Int`), and from each type
  # its included mixins and superclass, transitively — so `a.insp<TAB>` finds
  # `inspect` coming from `struct Int include Inspectable`.
  class MemberCompletionProvider < ACompletionProvider

    def initialize( @session : REPL::REPLSession )
    end

    def complete( context : CompletionContext ) : Array(CompletionCandidate)
      candidates = [] of CompletionCandidate
      receiver = context.receiver
      return candidates if receiver.nil?

      state = @session.state
      type = state.top_level_globals[ receiver ]?
      return candidates if type.nil?

      prefix = context.word
      seen = Set(String).new

      each_type_in_chain( state, type ) do |type_info|
        type_info.methods.each_key do |raw_name|
          method_name = suggestible_method_name( raw_name )
          next if method_name.nil?
          # Operators are called `receiver + arg`, never `receiver.+` — the
          # parser rejects a dot form, so a dot completion must not offer them.
          next unless identifier_method?( method_name )
          next unless method_name.starts_with?( prefix )
          next if seen.includes?( method_name )
          seen << method_name
          candidates << CompletionCandidate.new( method_name, :method )
        end
      end

      candidates
    end

    #------------------------------------------------------------------------------------

    # Walks the receiver type's full resolution chain — reopening names, then
    # each visited type's mixins and superclass — yielding every `TypeInfo`
    # found, each name at most once.
    private def each_type_in_chain( state : REPL::IncrementalState, type : Frontend::Type, & : Frontend::TypeInfo -> ) : Nil
      queue = type.reopen_names.dup
      type_name = type.to_s
      queue << type_name unless queue.includes?( type_name )

      visited = Set(String).new

      until queue.empty?
        name = queue.shift
        next if visited.includes?( name )
        visited << name

        type_info = state.types[ name ]?
        next if type_info.nil?

        yield type_info

        type_info.mixins.each { |mixin| queue << mixin }
        if superclass = type_info.superclass
          queue << superclass
        end
      end
    end

    private def identifier_method?( name : String ) : Bool
      first = name[ 0 ]?
      !first.nil? && ( first.letter? || first == '_' )
    end

  end


end
