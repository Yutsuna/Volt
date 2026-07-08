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
      case val.tag
      # `to_u64!` is the *unchecked* (wrapping) conversion : a native arg is
      # passed as the raw two's-complement bit pattern in the pointer's address
      # bits, so a negative `Int64` must reinterpret rather than range-check.
      # (`to_u64` raises `OverflowError` on any negative value — the old bug.)
      when IR::Value::Tag::Int then Pointer(Void).new(val.as_i.to_u64!)
      when IR::Value::Tag::Str then val.as_s.to_unsafe.as(Void*)
      else                          Pointer(Void).null
      end
    end

    # Resolves and caches the symbol pointer for native function `native_idx`
    # (Phase 5: `dlopen`+`dlsym` used to run on *every* `CALL_NATIVE`; now only
    # on the first call, cached in `@native_ptrs` indexed the same way as
    # `@unit.natives` — see `Vm#initialize`/`Vm#extend`).
    private def resolve_native(native_idx : Int32) : Void*
      cached = @native_ptrs[native_idx]
      return cached unless cached.null?

      native_func = @unit.natives[native_idx]
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

      @native_ptrs[native_idx] = ptr
      ptr
    end

    def call_native(native_idx : Int32, args : Slice( IR::Value )) : IR::Value
      ptr  = resolve_native(native_idx)
      argc = args.size
      if argc > 4
        raise VoltRuntimeError.new("Native FFI error: Tier-0 interpreter supports max 4 arguments for `#{@unit.natives[native_idx].name}`")
      end

      c_args = StaticArray(Void*, 4).new(Pointer(Void).null)
      argc.times { |i| c_args[i] = to_c_arg(args[i]) }
      wrapper = CFuncWrapper.new(ptr, Pointer(Void).null)

      res = case argc
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
      else
        c_func = pointerof(wrapper).as(CFunc4*).value
        c_func.call(c_args[0], c_args[1], c_args[2], c_args[3])
      end

      IR::Value.int(res.address.to_i64)
    end
  end

end
