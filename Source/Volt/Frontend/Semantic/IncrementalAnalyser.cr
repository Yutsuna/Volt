module Volt::Frontend


  class IncrementalTypeChecker < TypeChecker
    def initialize( sigs : SignatureTable, bag : DiagnosticBag,
                    types : Hash(String, TypeInfo), nominals : Hash(String, NominalType),
                    @repl_scope : Scope,
                    mono : Monomorphizer? = nil, collector : TypeCollector? = nil )
      super( sigs, bag, types, nominals, mono, collector )
    end

    def check_top_level( nodes : Array(ANode) ) : Nil
      check_body( nodes, @repl_scope )
    end
  end


  class IncrementalAnalyser
    getter bag : DiagnosticBag
    getter sigs : SignatureTable
    getter functions : Array(FuncDecl)
    getter top_level : Array(ANode)
    getter types : Hash(String, TypeInfo)
    getter nominals : Hash(String, NominalType)
    getter methods : Array(FuncDecl)
    getter method_entries : Array({FuncDecl, String, FuncSig})

    def initialize( @program : Program, @state : Volt::REPL::IncrementalState )
      @bag       = DiagnosticBag.new
      @sigs      = SignatureTable.new_with_existing( @bag, @state.signatures )

      # Fresh monomorphizer per input, seeded from the persisted state :
      # templates from earlier inputs stay instantiable, and combinations
      # already declared in a previous turn are pre-marked so they resolve
      # against the carried-over type instead of re-declaring. (A combination
      # from a *failed* input never reached the state, so it correctly isn't
      # pre-marked and re-instantiates cleanly.)
      @mono      = Monomorphizer.new( @bag )
      @collector = nil.as( TypeCollector? )
      @state.class_templates.each { |name, decl| @mono.class_templates[ name ] = decl }
      @state.func_templates.each { |name, decl| @mono.func_templates[ name ] = decl }
      @state.types.each_key { |name| @mono.mark_instantiated( name ) if name.includes?( '[' ) }
      @state.signatures.each_key { |name| @mono.mark_instantiated( name ) if name.includes?( '[' ) }

      @functions = [] of FuncDecl
      @top_level = [] of ANode
      @types     = @state.types.dup
      @nominals  = @state.nominals.dup
      @methods   = [] of FuncDecl
      @method_entries = [] of { FuncDecl, String, FuncSig }
      @final_scope = nil.as( Scope? )
    end

    def analyse : TypedProgram
      partition
      check
      promote_top_level_vars
      raise CompilationError.new( @bag ) if @bag.errors?

      # Update the IncrementalState with the successfully checked new items!
      @sigs.table.each do |name, sig|
        @state.add_signature( name, sig )
      end
      @types.each do |name, type_info|
        @state.add_type( name, type_info )
      end
      @nominals.each do |name, nominal_type|
        @state.add_nominal( name, nominal_type )
      end
      @mono.class_templates.each do |name, decl|
        @state.class_templates[ name ] = decl
      end
      @mono.func_templates.each do |name, decl|
        @state.func_templates[ name ] = decl
      end

      TypedProgram.new( @program, @functions, @top_level, @sigs.table, @types, @methods )
    end

    private def partition : Nil
      collector  = TypeCollector.new( @bag, @mono )
      @collector = collector

      # Pre-populate types and nominals from the state
      collector.types.merge!( @state.types )
      collector.nominals.merge!( @state.nominals )

      # Pre-populate resolved list with all existing types from the state so we don't re-resolve them
      @state.types.each_key do |name|
        collector.resolved << name
      end

      # For each class/struct/mixin/module in the state, create a dummy decl in @decls
      # so that sub-classing or references in TypeCollector won't complain or fail checks.
      @state.types.each do |name, info|
        loc = Span.new( "<repl-state>", 0_u32, 0_u32, 0_u32, 0_u32 )
        methods_nodes = info.methods_ast.values.map { |m| m.as( ANode ) }
        decl =
          case info.kind
          when .class?
            ClassDecl.new( name, [] of String, info.superclass, info.mixins, methods_nodes, [] of Annotation, info.is_abstract, loc )
          when .struct?
            StructDecl.new( name, info.mixins, methods_nodes, [] of Annotation, loc )
          when .mixin?
            MixinDecl.new( name, methods_nodes, loc )
          else # Module
            ModuleDecl.new( name, methods_nodes, loc )
          end
        collector.decls[ name ] = decl
      end

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
          # Same template split as `Analyser#partition`.
          if node.type_params.empty?
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

      # Same rule as `Analyser#partition` : a Core body shadowed by a user
      # redefinition in this very input must not be checked or compiled.
      @functions.select! do |fn|
        sig = @sigs[ fn.name ]?
        sig.nil? || sig.decl_span == fn.loc
      end
    end

    private def check : Nil
      # Pre-populate scope
      repl_scope = Scope.new
      @state.top_level_globals.each do |name, type|
        repl_scope.define( name, type )
      end

      # We construct our IncrementalTypeChecker
      checker = IncrementalTypeChecker.new( @sigs, @bag, @types, @nominals, repl_scope, @mono, @collector )

      # We check functions
      @functions.each do |fn|
        if sig = @sigs[ fn.name ]?
          checker.check_function( fn, sig )
        end
      end

      # We check top-level expressions/statements
      checker.check_top_level( @top_level )

      # We check methods, then drain generic instantiations to fixpoint :
      # same worklist as `Analyser#check`.
      checked = 0
      loop do
        while checked < @method_entries.size
          fn, owner, sig = @method_entries[ checked ]
          checked += 1
          checker.check_method( fn, sig, owner )
        end
        break if @mono.fresh_funcs.empty?
        fn = @mono.fresh_funcs.shift
        @sigs.collect( fn, @nominals ) unless @sigs[ fn.name ]?
        @functions << fn
        if sig = @sigs[ fn.name ]?
          checker.check_function( fn, sig )
        end
      end

      # Keep a reference to the checked scope so we can promote the top level variables
      @final_scope = repl_scope
    end

    private def promote_top_level_vars : Nil
      if scope = @final_scope
        scope.vars.each do |name, type|
          @state.add_global( name, type )
        end
      end
    end

    private def type_name( node : ANode ) : String
      node.class.name.split( "::" ).last
    end
  end


end
