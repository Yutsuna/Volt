module Volt


  module MDumpable

    #--------------------------------------------------------------------------

    def to_s( io : IO ) : Nil
      dump(io)
    end

    #--------------------------------------------------------------------------

    private macro dump( io )
      {{io}} << self.class.name << "\n{\n"
      {% for ivar in @type.instance_vars %}
        {{io}} << "  " << {{ivar.name.stringify}} << ":\t  " << @{{ivar.name.id}} << '\n'
      {% end %}
      {{io}} << "}"
    end

    #--------------------------------------------------------------------------

  end


end
