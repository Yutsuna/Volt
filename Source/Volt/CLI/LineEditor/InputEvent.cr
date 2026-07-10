require "./KeyEvent"


module Volt::CLI


  struct InputEvent
    property key  : KeyEvent
    property char : Char?
    property text : String?

    def initialize( @key, @char = nil, @text = nil )
    end
  end


end
