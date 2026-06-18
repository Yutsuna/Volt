module Volt
  module Codegen


    class FCodegen

      #--------------------------------------------------------------------------
      # Function generation
      #--------------------------------------------------------------------------

      private def gen_function ( symbol : String, params : Array(Ast::Param),
                                body : Array(Ast::Node), ret : Types::Type ) : String
        reset_function
        params.each do |p|
          ensure_alloca(p.name, p.ptype.llvm)
          emit "store #{p.ptype.llvm} %p.#{p.name}, #{p.ptype.llvm}* %s.#{p.name}"
        end
        body.each { |n| gen_node(n) }
        emit_default_ret(ret) unless @terminated

        sig = params.map { |p| "#{p.ptype.llvm} %p.#{p.name}" }.join(", ")
        build_define(symbol, ret.llvm, sig)
      end

      private def gen_main : String
        reset_function
        if @program.top_level.empty? && @defs.has_key?("main")
          t = new_temp
          emit "#{t} = call i32 @\"volt.main\"()"
          emit "ret i32 #{t}"
          @terminated = true
        else
          @program.top_level.each { |n| gen_node(n) }
          emit "ret i32 0" unless @terminated
        end
        build_define("@main", "i32", "")
      end

      private def build_define ( symbol : String, ret_llvm : String, sig : String ) : String
        String.build do |io|
          io << "define #{ret_llvm} #{symbol}(#{sig}) {\n"
          io << "entry:\n"
          @allocas.each { |a| io << "  " << a << "\n" }
          @body.each { |b| io << b << "\n" }
          io << "}\n"
        end
      end

      private def emit_default_ret ( ret : Types::Type ) : Nil
        if ret.void?
          emit "ret void"
        else
          emit "ret #{ret.llvm} #{zero_value(ret)}"
        end
      end

      private def zero_value ( ty : Types::Type ) : String
        return "null" if ty.pointer?
        return "0x0000000000000000" if ty.float?
        return "false" if ty.base == Types::EType::Bool
        "0"
      end

      #--------------------------------------------------------------------------

    end


  end
end
