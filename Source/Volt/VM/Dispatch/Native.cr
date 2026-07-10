@[Link("dl")]
lib LibDL
  RTLD_LAZY = 1
  fun dlopen(file : UInt8*, mode : Int32) : Void*
  fun dlsym(handle : Void*, name : UInt8*) : Void*
end

module Volt::VM

  # Universal FFI signature. On the SysV (x86-64) and AAPCS64 ABIs the
  # floating-point and integer argument registers are assigned independently
  # (xmm0-3 / v0-v3 vs rdi-rcx / x0-x3), so ONE Proc type whose first four
  # params are Float64 and next four are Void* can call any C function taking
  # up to 4 float and 4 integer/pointer arguments : the callee reads each of
  # its declared params from the register class its own prototype dictates,
  # regardless of how the extra (garbage) registers were filled here.
  # Variadic callees stay unsupported (the AL vector count is not set).
  alias CFuncUni = Float64, Float64, Float64, Float64, Void*, Void*, Void*, Void* -> Void*

  # Exact mirror structure of the internal structure of a Crystal Proc (16 bytes)
  # Allows recreating the wrapper on the stack without touching the execution machine code.
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
      when IR::Value::Tag::Int                     then Pointer(Void).new(val.as_i.to_u64!)
      when IR::Value::Tag::Ptr, IR::Value::Tag::Regex,
           IR::Value::Tag::Object, IR::Value::Tag::Bool then val.as_ptr
      else                                         Pointer(Void).null
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
      ptr = resolve_native(native_idx)

      # Split the Volt args by register class (see `CFuncUni`) : floats fill
      # the xmm/v slots in order, everything else the integer slots in order.
      f_args = StaticArray(Float64, 4).new(0.0)
      i_args = StaticArray(Void*, 4).new(Pointer(Void).null)
      fc = 0
      ic = 0
      args.each do |arg|
        if arg.tag.float?
          raise VoltRuntimeError.new("Native FFI error: Tier-0 interpreter supports max 4 float arguments for `#{@unit.natives[native_idx].name}`") if fc == 4
          f_args[fc] = arg.as_f
          fc += 1
        else
          raise VoltRuntimeError.new("Native FFI error: Tier-0 interpreter supports max 4 integer/pointer arguments for `#{@unit.natives[native_idx].name}`") if ic == 4
          i_args[ic] = to_c_arg(arg)
          ic += 1
        end
      end

      wrapper = CFuncWrapper.new(ptr, Pointer(Void).null)
      c_func  = pointerof(wrapper).as(CFuncUni*).value
      res = c_func.call(f_args[0], f_args[1], f_args[2], f_args[3],
                        i_args[0], i_args[1], i_args[2], i_args[3])

      IR::Value.int(res.address.to_i64)
    end
  end

end

# Runtime helper for Volt regex matching, resolved via FFI.
fun __volt_regex_match(regex : Void*, str : UInt8*, len : Int64) : Bool
  rx = regex.unsafe_as(Regex)
  slice = Slice.new(str, len)
  s = String.new(slice)
  rx.matches?(s)
end

# Runtime helper for Volt float formatting, resolved via FFI.
fun volt_format_float(val : Float64, buf : UInt8*) : Int32
  s = val.to_s
  s.to_unsafe.copy_to(buf, s.bytesize + 1)
  s.bytesize
end
