module Volt::Frontend


  struct Label
    getter span    : Span
    getter message : String
    getter primary : Bool

    def initialize( @span : Span, @message : String, @primary : Bool )
    end

    def self.primary( span : Span, message : String = "" ) : Label
      new( span, message, true )
    end

    def self.secondary( span : Span, message : String = "" ) : Label
      new( span, message, false )
    end
  end


end
