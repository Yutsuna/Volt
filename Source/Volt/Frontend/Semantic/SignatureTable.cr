module Volt::Frontend


  class SignatureTable
    getter table : Hash( String, FuncSig )

    def initialize( @bag : DiagnosticBag )
      @table = {} of String => FuncSig
    end

    def []?( name : String ) : FuncSig?
      @table[ name ]?
    end

    def names : Array( String )
      @table.keys
    end

    def collect( decl : FuncDecl, nominals : Hash( String, NominalType )? = nil ) : Nil
      return if duplicate?( decl.name, decl.loc )

      params = decl.params.map do |p|
        if ann = p.type_ann
          resolve_param( p.name, ann, nominals )
        else
          @bag << Catalog::Sema.param_needs_type( p.name, decl.name, p.loc )
          Type::UNKNOWN
        end
      end

      ret = Type::UNKNOWN
      if rt = decl.return_type
        ret = resolve_return( decl.name, rt, nominals )
      end

      @table[ decl.name ] = FuncSig.new( decl.name, params, ret, decl_span: decl.loc )
    end

    def collect_extern( decl : ExternDecl ) : Nil
      return if duplicate?( decl.name, decl.loc )

      params = decl.params.map do |p|
        if ann = p.type_ann
          resolve_param( p.name, ann )
        else
          Type::UNKNOWN
        end
      end

      ret = resolve_return( decl.name, decl.return_type )
      @table[ decl.name ] = FuncSig.new( decl.name, params, ret, extern: true, lib: decl.lib, decl_span: decl.loc )
    end

    # -----------------------------------------------------------------------------------

    private def duplicate?( name : String, span : Span ) : Bool
      if existing = @table[ name ]?
        @bag << Catalog::Sema.duplicate_definition( name, span, existing.decl_span )
        return true
      end
      false
    end

    private def resolve_param( name : String, ann : ATypeNode, nominals : Hash( String, NominalType )? = nil ) : Type
      ty = Type.from_annotation( ann, nominals )
      if ty.nil?
        @bag << Catalog::Sema.unsupported_param_type( name, ann.loc )
        return Type::UNKNOWN
      end
      ty
    end

    private def resolve_return( func : String, ann : ATypeNode, nominals : Hash( String, NominalType )? = nil ) : Type
      ty = Type.from_annotation( ann, nominals )
      if ty.nil?
        @bag << Catalog::Sema.unsupported_return_type( func, ann.loc )
        return Type::UNKNOWN
      end
      ty
    end
  end


end
