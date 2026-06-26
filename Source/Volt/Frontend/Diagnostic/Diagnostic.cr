module Volt::Frontend


  class Diagnostic
    getter code     : String
    getter severity : Severity
    getter message  : String
    getter labels   : Array( Label )
    getter notes    : Array( String )

    def initialize( @code : String, @severity : Severity, @message : String )
      @labels = [] of Label
      @notes  = [] of String
    end

    def self.error( code : String, message : String ) : Diagnostic
      new( code, Severity::Error, message )
    end

    def self.warning( code : String, message : String ) : Diagnostic
      new( code, Severity::Warning, message )
    end

    def with_label( label : Label ) : Diagnostic
      @labels << label
      self
    end

    def with_primary( span : Span, message : String = "" ) : Diagnostic
      with_label( Label.primary( span, message ) )
    end

    def with_secondary( span : Span, message : String = "" ) : Diagnostic
      with_label( Label.secondary( span, message ) )
    end

    def with_help( text : String ) : Diagnostic
      @notes << text
      self
    end

    def primary_span : Span?
      ( @labels.find( &.primary ) || @labels.first? ).try( &.span )
    end
  end


end
