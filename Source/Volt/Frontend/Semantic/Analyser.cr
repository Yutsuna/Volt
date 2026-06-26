module Volt::Frontend


  class Analyser
    def initialize( @program : Program )
      @bag       = DiagnosticBag.new
      @sigs      = SignatureTable.new( @bag )
      @functions = [] of FuncDecl
      @top_level = [] of ANode
    end

    def analyse : TypedProgram
      partition
      check
      raise CompilationError.new( @bag ) if @bag.errors?
      TypedProgram.new( @program, @functions, @top_level, @sigs.table )
    end

    private def partition : Nil
      @program.nodes.each do |node|
        case node
        when ExternDecl
          @sigs.collect_extern( node )
        when FuncDecl
          @sigs.collect( node )
          @functions << node
        when AExpr
          @top_level << node
        else
          @bag << Catalog::Sema.unsupported_top_level( type_name( node ), node.loc )
        end
      end
    end

    private def check : Nil
      checker = TypeChecker.new( @sigs, @bag )
      @functions.each do |fn|
        if sig = @sigs[ fn.name ]?
          checker.check_function( fn, sig )
        end
      end
      checker.check_top_level( @top_level )
    end

    private def type_name( node : ANode ) : String
      node.class.name.split( "::" ).last
    end
  end


end
