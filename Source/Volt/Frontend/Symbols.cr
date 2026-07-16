module Volt::Frontend


  # Process-global symbol intern pool. A `:name` literal is an `Int64` id at
  # runtime ("stocké sous forme d'entier en mémoire") : the parser interns
  # every `SymbolLit` it produces, the compiler emits the id as an integer
  # constant, and `Analyser` synthesizes `__symbol_name( id )` from this pool
  # so `Symbol#to_string` can resolve the name back at runtime.
  #
  # Ids are first-seen ordered and stable for the whole process — REPL turns
  # and multi-program spec runs share one pool, which only ever grows.
  module Symbols
    extend self

    @@ids   = {} of String => Int64
    @@names = [] of String

    def intern( name : String ) : Int64
      @@ids[ name ]? || begin
        @@names << name
        @@ids[ name ] = ( @@names.size - 1 ).to_i64
      end
    end

    # Snapshot of every interned name, indexed by id.
    def names : Array( String )
      @@names
    end

    # Volt source of `__symbol_name( id ) -> String`, the id → name table
    # `Symbol#to_string` resolves through at runtime. Regenerated from the
    # whole pool on every analysis (both `Analyser` and the REPL's
    # `IncrementalAnalyser` inject it — the Core's `Symbol.vl` references it
    # unconditionally, and a REPL turn may intern new symbols at any time).
    def function_source : String
      # No `-> String` annotation on purpose : the function is injected into
      # *every* analysed program, including bare Core-less unit-spec sources
      # where the name `String` doesn't resolve — the return type is inferred
      # from the literal tail instead, which works in both worlds.
      String.build do |s|
        s << "def __symbol_name( id : Int )\n"
        @@names.each_with_index do |name, i|
          s << "  return \"" << name << "\" if id == " << i << "\n"
        end
        s << "  \"<symbol:?>\"\n"
        s << "end\n"
      end
    end
  end


end
