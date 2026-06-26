module Volt::VM


  # Native (extern) call dispatch for the Tier-0 interpreter. This is the layer that
  # actually *implements* the externs declared in Volt via `@[External]`, so the registry
  # lives here — the compiler only forwards the name it interned in the `Unit`. Tier-0
  # provides a tiny set directly; real `dlopen`/`dlsym` FFI arrives with the JIT.
  class Vm
    def call_native( name : String, args : Array( IR::Value ) ) : IR::Value
      case name
      when "puts"
        args.each { |a| STDOUT.puts( a.to_display ) }
        STDOUT.puts if args.empty?
      when "print"
        args.each { |a| STDOUT.print( a.to_display ) }
      else
        raise VoltRuntimeError.new( "no native implementation for extern `#{name}`" )
      end
      IR::Value.nil_value
    end
  end


end
