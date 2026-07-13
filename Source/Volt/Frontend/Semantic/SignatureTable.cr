module Volt::Frontend


  class SignatureTable
    getter table : Hash( String, FuncSig )

    def initialize( @bag : DiagnosticBag )
      @table = {} of String => FuncSig
      # Names carried over from a previous REPL turn. A redefinition of one of
      # these is allowed (it shadows the old signature); a redefinition of a
      # name introduced in the *current* input is still a duplicate error.
      @redefinable = Set( String ).new
    end

    def self.new_with_existing( bag : DiagnosticBag, existing : Hash( String, FuncSig ) ) : SignatureTable
      st = new( bag )
      st.table.merge!( existing )
      existing.each_key { |name| st.mark_redefinable( name ) }
      st
    end

    def mark_redefinable( name : String ) : Nil
      @redefinable << name
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
      sig = FuncSig.new( decl.name, params, ret, extern: true, lib: decl.lib, decl_span: decl.loc )
      sig.extern_name = decl.extern_name
      @table[ decl.name ] = sig
    end

    # -----------------------------------------------------------------------------------

    private def duplicate?( name : String, span : Span ) : Bool
      if existing = @table[ name ]?
        # A name inherited from a prior REPL turn may be redefined once; after
        # that the fresh definition owns the slot and further collisions error.
        if @redefinable.delete( name )
          return false
        end
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
