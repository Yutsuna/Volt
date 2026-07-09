module Volt::Frontend


  class Analyser
    def initialize( @program : Program )
      @bag       = DiagnosticBag.new
      @sigs      = SignatureTable.new( @bag )
      @functions = [] of FuncDecl
      @top_level = [] of ANode
      @types     = {} of String => TypeInfo
      @nominals  = {} of String => NominalType
      @methods   = [] of FuncDecl
      @method_entries = [] of { FuncDecl, String, FuncSig }
    end

    def analyse : TypedProgram
      partition
      check
      raise CompilationError.new( @bag ) if @bag.errors?
      TypedProgram.new( @program, @functions, @top_level, @sigs.table, @types, @methods )
    end

    private def partition : Nil
      collector = TypeCollector.new( @bag )
      collector.collect( @program.nodes )
      @types          = collector.types
      @nominals       = collector.nominals
      @methods        = collector.methods
      @method_entries = collector.method_entries

      @program.nodes.each do |node|
        case node
        when ExternDecl
          @sigs.collect_extern( node )
        when FuncDecl
          @sigs.collect( node, @nominals )
          @functions << node
        when AExpr
          @top_level << node
        when ClassDecl, StructDecl, MixinDecl, ModuleDecl, CircuitDecl
          # handled above by TypeCollector
        else
          @bag << Catalog::Sema.unsupported_top_level( type_name( node ), node.loc )
        end
      end
    end

    private def check : Nil
      checker = TypeChecker.new( @sigs, @bag, @types, @nominals )
      @functions.each do |fn|
        if sig = @sigs[ fn.name ]?
          checker.check_function( fn, sig )
        end
      end
      checker.check_top_level( @top_level )
      @method_entries.each do |fn, owner, sig|
        checker.check_method( fn, sig, owner )
      end
    end

    private def type_name( node : ANode ) : String
      node.class.name.split( "::" ).last
    end
  end


end
