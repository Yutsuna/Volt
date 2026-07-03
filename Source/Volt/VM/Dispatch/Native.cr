# Source/VM/Dispatch/Native.cr

# Interface native du système (POSIX) pour le chargement dynamique (FFI)
@[Link("dl")]
lib LibDL
  RTLD_LAZY = 1
  fun dlopen(file : UInt8*, mode : Int32) : Void*
  fun dlsym(handle : Void*, name : UInt8*) : Void*
end

module Volt::VM

  # Signatures FFI génériques représentées par des pointeurs machine
  alias CFunc0 = -> Void*
  alias CFunc1 = Void* -> Void*
  alias CFunc2 = Void*, Void* -> Void*
  alias CFunc3 = Void*, Void*, Void* -> Void*
  alias CFunc4 = Void*, Void*, Void*, Void* -> Void*

  # Structure miroir exacte de la structure interne d'une Proc Crystal (16 octets)
  # Permet de recréer l'enveloppe sur la stack sans toucher au code machine d'exécution.
  struct CFuncWrapper
    property fptr : Void*
    property env : Void*

    def initialize(@fptr, @env)
    end
  end

  class Vm

    private def to_c_arg(val : IR::Value) : Void*
      case val.raw
      when Int64  then Pointer(Void).new(val.as_i.to_u64)
      when String then val.as_s.to_unsafe.as(Void*)
      when Nil    then Pointer(Void).null
      else             Pointer(Void).null
      end
    end

    def call_native(native_func : Compiler::NativeFunc, args : Array( IR::Value )) : IR::Value
      lib_name  = native_func.lib
      func_name = native_func.name

      handle = Pointer(Void).null

      if lib_name
        handle = LibDL.dlopen(lib_name.to_unsafe, LibDL::RTLD_LAZY)
        if handle.null? && lib_name == "libc"
          handle = LibDL.dlopen("libc.so.6".to_unsafe, LibDL::RTLD_LAZY)
          handle = LibDL.dlopen("libc.dylib".to_unsafe, LibDL::RTLD_LAZY) if handle.null?
        end
      end

      handle = LibDL.dlopen(Pointer(UInt8).null, LibDL::RTLD_LAZY) if handle.null?

      if handle.null?
        raise VoltRuntimeError.new("Cannot open library `#{lib_name}` for external function `#{func_name}`")
      end

      ptr = LibDL.dlsym(handle, func_name.to_unsafe)
      if ptr.null?
        raise VoltRuntimeError.new("Unresolved external symbol: `#{func_name}` in `#{lib_name || "process image"}`")
      end

      c_args = args.map { |a| to_c_arg(a) }
      wrapper = CFuncWrapper.new(ptr, Pointer(Void).null)

      res = case c_args.size
      when 0
        c_func = pointerof(wrapper).as(CFunc0*).value
        c_func.call()
      when 1
        c_func = pointerof(wrapper).as(CFunc1*).value
        c_func.call(c_args[0])
      when 2
        c_func = pointerof(wrapper).as(CFunc2*).value
        c_func.call(c_args[0], c_args[1])
      when 3
        c_func = pointerof(wrapper).as(CFunc3*).value
        c_func.call(c_args[0], c_args[1], c_args[2])
      when 4
        c_func = pointerof(wrapper).as(CFunc4*).value
        c_func.call(c_args[0], c_args[1], c_args[2], c_args[3])
      else
        raise VoltRuntimeError.new("Native FFI error: Tier-0 interpreter supports max 4 arguments for `#{func_name}`")
      end

      IR::Value.int(res.address.to_i64)
    end
  end

end
