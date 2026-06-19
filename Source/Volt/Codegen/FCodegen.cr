require "./Codegen/TopLevels"
require "./Codegen/Expressions"
require "./Codegen/Statements"


module Volt
  module Codegen


    # Conventions:
    #   - extern C functions keep their verbatim name (`@puts`).
    #   - Volt defs are mangled (`@"volt.name"`) to avoid clashing with C `main`.
    #   - top-level statements form the synthesized C `define i32 @main()`.
    #   - variable slots live in `%s.<slot>` allocas; SSA temps are `%tN`.
    class FCodegen

      def initialize ( @program : Ast::Program, @reporter : Diagnostic::FReporter )
        @emitter        = FEmitter.new
        @externs        = {} of String => Ast::ExternDef
        @defs           = {} of String => Ast::Def
        @used_builtins  = Set(String).new

        # Per-function state (reset for each function).
        @allocas   = [] of String
        @body      = [] of String
        @allocated = Set(String).new
        @tmp       = 0
        @label     = 0
        @seq       = 0
        @terminated = false
      end

      #--------------------------------------------------------------------------

      def generate : String
        @program.externs.each { |e| @externs[e.name] = e }
        @program.defs.each { |d| @defs[d.name] = d }

        functions = [] of String
        @program.defs.each do |d|
          functions << gen_function("@\"volt.#{d.name}\"", d.params, d.body, d.return_type)
        end
        functions << gen_main

        assemble(functions)
      end

      #--------------------------------------------------------------------------

      private def assemble ( functions : Array(String) ) : String
        String.build do |io|
          @emitter.declarations.each { |d| io << d << "\n" }
          io << "\n" unless @emitter.declarations.empty?

          @program.externs.each do |e|
            io << "declare #{e.return_type.llvm} @#{e.name}(#{param_types(e.params)})\n"
          end
          if @used_builtins.includes?("puts") && !@externs.has_key?("puts")
            io << "declare i32 @puts(i8*)\n"
          end
          if @used_builtins.includes?("exit") && !@externs.has_key?("exit")
            io << "declare void @exit(i32)\n"
          end
          io << "\n"

          functions.each { |f| io << f << "\n" }
        end
      end

      private def param_types ( params : Array(Ast::Param) ) : String
        params.map { |p| p.ptype.llvm }.join(", ")
      end

      #--------------------------------------------------------------------------
      # Helpers
      #--------------------------------------------------------------------------

      private def binary_instr ( op : String, ty : Types::Type ) : String
        float = ty.float?
        case op
        when "+"  then float ? "fadd" : "add"
        when "-"  then float ? "fsub" : "sub"
        when "*"  then float ? "fmul" : "mul"
        when "/"  then float ? "fdiv" : "sdiv"
        when "%"  then float ? "frem" : "srem"
        when "&"  then "and"
        when "|"  then "or"
        when "^"  then "xor"
        when "&&" then "and"
        when "||" then "or"
        when "<<" then "shl"
        when ">>" then "ashr"
        when "==" then float ? "fcmp oeq" : "icmp eq"
        when "!=" then float ? "fcmp one" : "icmp ne"
        when "<"  then float ? "fcmp olt" : "icmp slt"
        when ">"  then float ? "fcmp ogt" : "icmp sgt"
        when "<=" then float ? "fcmp ole" : "icmp sle"
        when ">=" then float ? "fcmp oge" : "icmp sge"
        else           "add"
        end
      end

      private def resolve_symbol ( name : String ) : {String, Types::Type}
        if d = @defs[name]?
          return {"@\"volt.#{name}\"", d.return_type}
        end
        if e = @externs[name]?
          return {"@#{name}", e.return_type}
        end
        case name
        when "puts"
          @used_builtins << "puts"
          {"@puts", Types::Type.new(Types::EType::Int32)}
        when "exit"
          @used_builtins << "exit"
          {"@exit", Types::Type.new(Types::EType::Nil)}
        else
          {"@#{name}", Types::Type.new(Types::EType::Int32)}
        end
      end

      private def f64_hex ( value : Float64, type : Types::Type? = nil ) : String
        if type && type.base == Types::EType::Float32
          bits = value.to_f32.to_f64.unsafe_as( UInt64 )
        else
          bits = value.unsafe_as( UInt64 )
        end
        "0x%016X" % bits
      end

      private def type_of ( expr : Ast::Expr ) : Types::Type
        expr.type || Types::Type.new(Types::EType::Int32)
      end

      #--------------------------------------------------------------------------
      # Per-function buffer management
      #--------------------------------------------------------------------------

      private def reset_function : Nil
        @allocas   = [] of String
        @body      = [] of String
        @allocated = Set(String).new
        @tmp        = 0
        @label      = 0
        @seq        = 0
        @terminated = false
      end

      private def ensure_alloca ( slot : String, llvm_type : String ) : Nil
        return if @allocated.includes?(slot)
        @allocated << slot
        @allocas << "%s.#{slot} = alloca #{llvm_type}"
      end

      private def emit ( line : String ) : Nil
        @body << "  #{line}"
      end

      private def emit_term ( line : String ) : Nil
        @body << "  #{line}"
        @terminated = true
      end

      private def start_block ( label : String ) : Nil
        @body << "#{label}:"
        @terminated = false
      end

      private def branch ( label : String ) : Nil
        @body << "  br label %#{label}"
        @terminated = true
      end

      private def new_temp : String
        @tmp += 1
        "%t#{@tmp}"
      end

      private def new_label : String
        @label += 1
        "bb#{@label}"
      end

      private def next_seq : Int32
        @seq += 1
        @seq
      end

    end


  end
end
