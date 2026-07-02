require "./KeyEvent"


module Volt::CLI


  struct InputEvent
    property key  : KeyEvent
    property char : Char?

    def initialize( @key, @char = nil )
    end
  end


end
