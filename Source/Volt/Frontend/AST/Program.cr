module Volt::Frontend


  class Program < ANode
    property nodes : Array( ANode )
    property file  : String

    def initialize( @nodes : Array( ANode ), @file : String, loc : Span )
      super( loc )
    end
  end


end
