module Volt::Frontend


  class Scope
    getter parent : Scope?

    def initialize( @parent : Scope? = nil )
      @vars = {} of String => Type
    end

    def child : Scope
      Scope.new( self )
    end

    def define( name : String, type : Type ) : Nil
      @vars[ name ] = type
    end

    def lookup( name : String ) : Type?
      @vars[ name ]? || @parent.try( &.lookup( name ) )
    end

    def local?( name : String ) : Bool
      @vars.has_key?( name )
    end

    def visible_names : Array( String )
      names = @vars.keys
      if p = @parent
        names.concat( p.visible_names )
      end
      names
    end
  end


end
