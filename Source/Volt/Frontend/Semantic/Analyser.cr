module Volt::Frontend


  class Analyser
    def initialize( @program : Program )
      @bag       = DiagnosticBag.new
      @sigs      = SignatureTable.new( @bag )
      @mono      = Monomorphizer.new( @bag )
      @collector = nil.as( TypeCollector? )
      @functions = [] of FuncDecl
      @top_level = [] of ANode
      @types     = {} of String => TypeInfo
      @nominals  = {} of String => NominalType
      @methods   = [] of FuncDecl
      @method_entries = [] of { FuncDecl, String, FuncSig }
    end

    def analyse : TypedProgram
      inject_symbol_names
      partition
      check
      raise CompilationError.new( @bag ) if @bag.errors?
      TypedProgram.new( @program, @functions, @top_level, @sigs.table, @types, @methods )
    end

    # Synthesizes `__symbol_name( id ) -> String` from the process-global
    # intern pool (`Frontend::Symbols`, filled by the parser) and appends it
    # to the program — this is how `Symbol#to_string` resolves an interned
    # `Int64` id back to its source name at runtime. Always injected (the
    # Core's `Symbol.vl` references it unconditionally) ; generated as source
    # and re-parsed so it flows through signature collection, typecheck and
    # codegen like any ordinary function. Marked as a core file so a user
    # program can never collide with the name.
    private def inject_symbol_names : Nil
      # Only meaningful when the Core is present : its `Symbol.vl` is the one
      # consumer, and a bare Core-less program (unit specs) can't even
      # compile the table's string literals.
      return unless @program.nodes.any? { |n| n.is_a?( StructDecl ) && n.name == "Symbol" }
      parsed = Frontend.parse( Symbols.function_source, "<symbols>" )
      Frontend.core_files << "<symbols>"
      @program.nodes.concat( parsed.nodes )
    end

    private def partition : Nil
      collector  = TypeCollector.new( @bag, @mono )
      @collector = collector
      collector.collect( @program.nodes )
      @types          = collector.types
      @nominals       = collector.nominals
      @methods        = collector.methods
      @method_entries = collector.method_entries

      @program.nodes.each do |node|
        case node
        when ExternDecl
          @sigs.collect_extern( node )
          @sigs.mark_redefinable( node.name ) if Frontend.core_file?( node.loc.file )
        when FuncDecl
          # A generic function (`def foo[T]` / `forall T`) is a template :
          # signature collection would resolve `T` to nothing, so it only
          # enters the table once instantiated with concrete types.
          if node.type_params.empty?
            # A concrete signature may still mention a generic reference
            # (`def show( p : Pair[String, Int64] )`) : instantiate those
            # before collection so the annotation resolves.
            node.params.each { |p| p.type_ann.try { |a| collector.instantiate_generics_in( a ) } }
            node.return_type.try { |a| collector.instantiate_generics_in( a ) }
            @sigs.collect( node, @nominals )
            @sigs.mark_redefinable( node.name ) if Frontend.core_file?( node.loc.file )
            @functions << node
          else
            @mono.register_func_template( node )
          end
        when AExpr
          @top_level << node
        when ClassDecl, StructDecl, MixinDecl, ModuleDecl, CircuitDecl
          # handled above by TypeCollector
        else
          @bag << Catalog::Sema.unsupported_top_level( type_name( node ), node.loc )
        end
      end

      # A shadowed Core function must not keep its body around : the surviving
      # signature is the user's, and `BytecodeCompiler` indexes chunks by name
      # — the stale body would be checked against the wrong signature and
      # compiled into a dead chunk.
      @functions.select! do |fn|
        sig = @sigs[ fn.name ]?
        sig.nil? || sig.decl_span == fn.loc
      end
    end

    private def check : Nil
      checker = TypeChecker.new( @sigs, @bag, @types, @nominals, @mono, @collector )
      @functions.each do |fn|
        if sig = @sigs[ fn.name ]?
          checker.check_function( fn, sig )
        end
      end
      checker.check_top_level( @top_level )

      # Worklist to fixpoint : checking a body can instantiate generics, which
      # appends method entries (fresh classes, already collected by the
      # TypeCollector drain) and queues fresh function clones. Both kinds of
      # new work can themselves instantiate further generics; the
      # monomorphizer's per-name cache guarantees termination.
      checked = 0
      loop do
        while checked < @method_entries.size
          fn, owner, sig = @method_entries[ checked ]
          checked += 1
          checker.check_method( fn, sig, owner )
        end
        break if @mono.fresh_funcs.empty?
        fn = @mono.fresh_funcs.shift
        # The checker may have collected the signature already (it needs the
        # return type at the instantiating call site).
        @sigs.collect( fn, @nominals ) unless @sigs[ fn.name ]?
        @functions << fn
        if sig = @sigs[ fn.name ]?
          checker.check_function( fn, sig )
        end
      end
    end

    private def type_name( node : ANode ) : String
      node.class.name.split( "::" ).last
    end
  end


end
