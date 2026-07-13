require "colorize"

module Volt::Frontend


  module ASTDump


    class_property show_loc : Bool = true
    class_property color    : Bool = false

    BRANCH = "├─ "
    CORNER = "└─ "
    VERT   = "│  "
    SPACE  = "   "

    def self.branch(last : Bool) : String
      s = last ? CORNER : BRANCH
      color ? s.colorize(:dark_gray).to_s : s
    end

    def self.vert(last : Bool) : String
      s = last ? SPACE : VERT
      color ? s.colorize(:dark_gray).to_s : s
    end

    def self.nd(name : String) : String
      color ? name.colorize(:white).bold.to_s : name
    end

    def self.key(k : String) : String
      color ? k.colorize(:dark_gray).to_s : k
    end

    def self.val(v : String) : String
      color ? v.colorize(:cyan).to_s : v
    end

    def self.lbl(l : String) : String
      color ? l.colorize(:blue).to_s : l
    end

    def self.loc(s : Span) : String
      return "" unless show_loc
      str = " @#{s.line}:#{s.column}"
      color ? str.colorize(:dark_gray).to_s : str
    end

    def self.op(k : TokenKind) : String
      case k
      when .plus?        then "+"
      when .minus?       then "-"
      when .star?        then "*"
      when .slash?       then "/"
      when .percent?     then "%"
      when .star_star?   then "**"
      when .eq_eq?       then "=="
      when .bang_eq?     then "!="
      when .lt?          then "<"
      when .gt?          then ">"
      when .lt_eq?       then "<="
      when .gt_eq?       then ">="
      when .amp_amp?     then "&&"
      when .amp?         then "&"
      when .pipe_pipe?   then "||"
      when .bang?        then "!"
      when .pipe_gt?     then "|>"
      when .dot_dot?     then ".."
      when .dot_dot_dot? then "..."
      else                    k.to_s.downcase
      end
    end
  end



  abstract class ANode
    def inspect(io : IO) : Nil
      dump(io, "")
    end

    abstract def dump(io : IO, prefix : String) : Nil


    protected def arm(last : Bool) : String  ; ASTDump.branch(last) ; end
    protected def cpfx(p : String, last : Bool) : String ; p + ASTDump.vert(last) ; end

    protected def section(io : IO, label : String, nodes : Array(ANode), prefix : String, last : Bool) : Nil
      io << prefix << arm(last) << ASTDump.lbl(label) << "\n"
      sub = cpfx(prefix, last)
      nodes.each_with_index do |n, i|
        child_last = i == nodes.size - 1
        io << sub << arm(child_last)
        n.dump(io, cpfx(sub, child_last))
      end
    end

    protected def field(io : IO, label : String, node : ANode, prefix : String, last : Bool) : Nil
      io << prefix << arm(last) << ASTDump.lbl(label) << "\n"
      sub = cpfx(prefix, last)
      io << sub << arm(true)
      node.dump(io, cpfx(sub, true))
    end
  end



  class Program
    def inspect(io : IO) : Nil ; dump(io, "") ; end

    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("Program") << " " << ASTDump.val("'#{@file}'") << "\n"
      @nodes.each_with_index do |n, i|
        last = i == @nodes.size - 1
        io << prefix << arm(last)
        n.dump(io, cpfx(prefix, last))
      end
    end
  end



  class Annotation
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("Annotation") << " " << ASTDump.val("'#{@name}'") << ASTDump.loc(@loc) << "\n"
      @args.each_with_index do |a, i|
        last = i == @args.size - 1
        io << prefix << (last ? ASTDump::CORNER : ASTDump::BRANCH)
        a.dump(io, prefix + (last ? ASTDump::SPACE : ASTDump::VERT))
      end
    end
  end

  class Param
    def dump(io : IO, prefix : String) : Nil
      shown = @is_ivar ? "@#{@name}" : @name
      io << ASTDump.nd("Param") << " " << ASTDump.val("'#{shown}'") << ASTDump.loc(@loc) << "\n"
      has_def = !@default.nil?
      if ta = @type_ann
        last = !has_def
        io << prefix << (last ? ASTDump::CORNER : ASTDump::BRANCH) << ASTDump.lbl("type") << "\n"
        sub = prefix + (last ? ASTDump::SPACE : ASTDump::VERT)
        io << sub << ASTDump::CORNER
        ta.dump(io, sub + ASTDump::SPACE)
      end
      if dv = @default
        io << prefix << ASTDump::CORNER << ASTDump.lbl("default") << "\n"
        sub = prefix + ASTDump::SPACE
        io << sub << ASTDump::CORNER
        dv.dump(io, sub + ASTDump::SPACE)
      end
    end
  end

  class FieldDecl
    def dump(io : IO, prefix : String) : Nil
      acc = ""
      acc += " #{ASTDump.key("getter")}" if @is_getter
      acc += " #{ASTDump.key("setter")}" if @is_setter
      io << ASTDump.nd("FieldDecl") << " " << ASTDump.val("'#{@name}'") << acc << ASTDump.loc(@loc) << "\n"
      has_val = !@value.nil?
      io << prefix << (has_val ? ASTDump::BRANCH : ASTDump::CORNER) << ASTDump.lbl("type") << "\n"
      sub_t = prefix + (has_val ? ASTDump::VERT : ASTDump::SPACE)
      io << sub_t << ASTDump::CORNER
      @type_ann.dump(io, sub_t + ASTDump::SPACE)
      if v = @value
        io << prefix << ASTDump::CORNER << ASTDump.lbl("value") << "\n"
        sub = prefix + ASTDump::SPACE
        io << sub << ASTDump::CORNER
        v.dump(io, sub + ASTDump::SPACE)
      end
    end
  end

  class IncludeDecl
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("IncludeDecl") << " " << ASTDump.val("'#{@name}'") << ASTDump.loc(@loc) << "\n"
    end
  end

  class ClassVarDecl
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("ClassVarDecl") << " " << ASTDump.val("'@@#{@name}'") << ASTDump.loc(@loc) << "\n"
      has_val = !@value.nil?
      io << prefix << (has_val ? ASTDump::BRANCH : ASTDump::CORNER) << ASTDump.lbl("type") << "\n"
      sub_t = prefix + (has_val ? ASTDump::VERT : ASTDump::SPACE)
      io << sub_t << ASTDump::CORNER
      @type_ann.dump(io, sub_t + ASTDump::SPACE)
      if v = @value
        io << prefix << ASTDump::CORNER << ASTDump.lbl("value") << "\n"
        sub = prefix + ASTDump::SPACE
        io << sub << ASTDump::CORNER
        v.dump(io, sub + ASTDump::SPACE)
      end
    end
  end

  class ExtendSelfDecl
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("ExtendSelfDecl") << ASTDump.loc(@loc) << "\n"
    end
  end

  class FuncDecl
    def dump(io : IO, prefix : String) : Nil
      async_s = @is_async ? " #{ASTDump.key("async")}" : ""
      abst_s  = @is_abstract ? " #{ASTDump.key("abstract")}" : ""
      stat_s  = @is_static ? " #{ASTDump.key("static")}" : ""
      vis_s   = @visibility.public? ? "" : " #{ASTDump.key(@visibility.to_s.downcase)}"
      tp_s    = @type_params.empty? ? "" : ASTDump.key("[#{@type_params.join(", ")}]")
      io << ASTDump.nd("FuncDecl") << abst_s << stat_s << vis_s << async_s << " " << ASTDump.val("'#{@name}'") << tp_s << ASTDump.loc(@loc) << "\n"

      has_ann  = !@annotations.empty?
      has_par  = !@params.empty?
      has_ret  = !@return_type.nil?
      has_body = !@body.empty?

      if has_ann
        last = !has_par && !has_ret && !has_body
        io << prefix << arm(last) << ASTDump.lbl("annotations") << "\n"
        sub = cpfx(prefix, last)
        @annotations.each_with_index do |a, i|
          al = i == @annotations.size - 1
          io << sub << (al ? ASTDump::CORNER : ASTDump::BRANCH)
          a.dump(io, sub + (al ? ASTDump::SPACE : ASTDump::VERT))
        end
      end

      if has_par
        last = !has_ret && !has_body
        io << prefix << arm(last) << ASTDump.lbl("params") << "\n"
        sub = cpfx(prefix, last)
        @params.each_with_index do |p, i|
          pl = i == @params.size - 1
          io << sub << (pl ? ASTDump::CORNER : ASTDump::BRANCH)
          p.dump(io, sub + (pl ? ASTDump::SPACE : ASTDump::VERT))
        end
      end

      if rt = @return_type
        field(io, "return_type", rt, prefix, !has_body)
      end

      section(io, "body", @body, prefix, true) if has_body
    end
  end

  class ClassDecl
    def dump(io : IO, prefix : String) : Nil
      abst_s  = @is_abstract ? " #{ASTDump.key( "abstract" )}" : ""
      tp_s    = @type_params.empty? ? "" : ASTDump.key( "[#{@type_params.join(", ")}]" )
      super_s = @superclass ? " " + ASTDump.key( "<" ) + " " + ASTDump.val( @superclass.to_s ) : ""
      mix_s   = @mixins.empty? ? "" : " " + ASTDump.key( "include" ) + " " + ASTDump.val( @mixins.join( ", " ) )
      io << ASTDump.nd( "ClassDecl" ) << abst_s << " " << ASTDump.val( "'#{@name}'" ) << tp_s << super_s << mix_s << ASTDump.loc( @loc ) << "\n"

      has_ann  = !@annotations.empty?
      has_body = !@body.empty?

      if has_ann
        io << prefix << arm(!has_body) << ASTDump.lbl("annotations") << "\n"
        sub = cpfx(prefix, !has_body)
        @annotations.each_with_index do |a, i|
          al = i == @annotations.size - 1
          io << sub << (al ? ASTDump::CORNER : ASTDump::BRANCH)
          a.dump(io, sub + (al ? ASTDump::SPACE : ASTDump::VERT))
        end
      end

      section(io, "body", @body, prefix, true) if has_body
    end
  end

  class MixinDecl
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("MixinDecl") << " " << ASTDump.val("'#{@name}'") << ASTDump.loc(@loc) << "\n"
      section(io, "body", @body, prefix, true) unless @body.empty?
    end
  end

  class StructDecl
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("StructDecl") << " " << ASTDump.val("'#{@name}'") << ASTDump.loc(@loc) << "\n"

      has_ann  = !@annotations.empty?
      has_body = !@body.empty?

      if has_ann
        io << prefix << arm(!has_body) << ASTDump.lbl("annotations") << "\n"
        sub = cpfx(prefix, !has_body)
        @annotations.each_with_index do |a, i|
          al = i == @annotations.size - 1
          io << sub << (al ? ASTDump::CORNER : ASTDump::BRANCH)
          a.dump(io, sub + (al ? ASTDump::SPACE : ASTDump::VERT))
        end
      end

      section(io, "body", @body, prefix, true) if has_body
    end
  end

  class ModuleDecl
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("ModuleDecl") << " " << ASTDump.val("'#{@name}'") << ASTDump.loc(@loc) << "\n"
      section(io, "body", @body, prefix, true) unless @body.empty?
    end
  end

  class ComponentDecl
    def dump(io : IO, prefix : String) : Nil
      async_s = @is_async ? " #{ASTDump.key("async")}" : ""
      io << ASTDump.nd("ComponentDecl") << async_s << " " << ASTDump.val("'#{@name}'") << ASTDump.loc(@loc) << "\n"

      has_par  = !@params.empty?
      has_body = !@body.empty?

      if has_par
        io << prefix << arm(!has_body) << ASTDump.lbl("params") << "\n"
        sub = cpfx(prefix, !has_body)
        @params.each_with_index do |p, i|
          pl = i == @params.size - 1
          io << sub << (pl ? ASTDump::CORNER : ASTDump::BRANCH)
          p.dump(io, sub + (pl ? ASTDump::SPACE : ASTDump::VERT))
        end
      end

      section(io, "body", @body, prefix, true) if has_body
    end
  end

  class UseDecl
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("UseDecl") << " " << ASTDump.val("'#{@path}'") << ASTDump.loc(@loc) << "\n"
    end
  end

  class ExternDecl
    def dump(io : IO, prefix : String) : Nil
      lib_s = @lib ? " #{ASTDump.key("lib=")}#{ASTDump.val("'#{@lib}'")}" : ""
      io << ASTDump.nd("ExternDecl") << lib_s << " " << ASTDump.key("name=") << ASTDump.val("'#{@name}'") << ASTDump.loc(@loc) << "\n"

      has_par = !@params.empty?

      if has_par
        io << prefix << arm(false) << ASTDump.lbl("params") << "\n"
        sub = prefix + ASTDump::VERT
        @params.each_with_index do |p, i|
          pl = i == @params.size - 1
          io << sub << (pl ? ASTDump::CORNER : ASTDump::BRANCH)
          p.dump(io, sub + (pl ? ASTDump::SPACE : ASTDump::VERT))
        end
      end

      field(io, "return_type", @return_type, prefix, true)
    end
  end

  class CircuitDecl
    def dump(io : IO, prefix : String) : Nil
      rt_s = @runtime ? " #{ASTDump.key("runtime=")}#{ASTDump.val("'#{@runtime}'")}" : ""
      ep_s = @entrypoint ? " #{ASTDump.key("entrypoint=")}#{ASTDump.val("'#{@entrypoint}'")}" : ""
      io << ASTDump.nd("CircuitDecl") << " " << ASTDump.val("'#{@name}'") << rt_s << ep_s << ASTDump.loc(@loc) << "\n"
      if m = @modules
        field(io, "modules", m, prefix, true)
      end
    end
  end

  class LinkDecl
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("LinkDecl") << " " << ASTDump.val("'#{@module_name}'") << ASTDump.loc(@loc) << "\n"
    end
  end



  class IntLit
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("IntLit") << " " << ASTDump.val(@value.to_s) << ASTDump.loc(@loc) << "\n"
    end
  end

  class FloatLit
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("FloatLit") << " " << ASTDump.val(@value.to_s) << ASTDump.loc(@loc) << "\n"
    end
  end

  class StringLit
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("StringLit") << " " << ASTDump.val(@value.inspect) << ASTDump.loc(@loc) << "\n"
    end
  end

  class BoolLit
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("BoolLit") << " " << ASTDump.val(@value.to_s) << ASTDump.loc(@loc) << "\n"
    end
  end

  class NilLit
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("NilLit") << ASTDump.loc(@loc) << "\n"
    end
  end

  class ArrayLit
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("ArrayLit") << ASTDump.loc(@loc) << "\n"
      section(io, "elements", @elements.map(&.as(ANode)), prefix, true) unless @elements.empty?
    end
  end

  class HashLiteralExpr
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("HashLiteralExpr") << ASTDump.loc(@loc) << "\n"
      @pairs.each_with_index do |(k, v), i|
        last = i == @pairs.size - 1
        io << prefix << arm(last) << ASTDump.lbl("pair") << "\n"
        sub = cpfx(prefix, last)
        field(io, "key",   k, sub, false)
        field(io, "value", v, sub, true)
      end
    end
  end

  class RegexLit
    def dump( io : IO, prefix : String ) : Nil
      io << ASTDump.nd( "RegexLit" ) << " " << ASTDump.val( "/" + @value + "/" ) << ASTDump.loc( @loc ) << "\n"
    end
  end

  class Ident
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("Ident") << " " << ASTDump.val("'#{@name}'") << ASTDump.loc(@loc) << "\n"
    end
  end

  class SelfExpr
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("SelfExpr") << ASTDump.loc(@loc) << "\n"
    end
  end

  class SuperCall
    def dump(io : IO, prefix : String) : Nil
      implicit_s = @implicit_args ? " #{ASTDump.key("implicit-args")}" : ""
      io << ASTDump.nd("SuperCall") << implicit_s << ASTDump.loc(@loc) << "\n"
      section(io, "args", @args.map(&.as(ANode)), prefix, true) unless @args.empty?
    end
  end

  class InstanceVar
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("InstanceVar") << " " << ASTDump.val("'@#{@name}'") << ASTDump.loc(@loc) << "\n"
    end
  end

  class ClassVar
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("ClassVar") << " " << ASTDump.val("'@@#{@name}'") << ASTDump.loc(@loc) << "\n"
    end
  end

  class MemberAccess
    def dump(io : IO, prefix : String) : Nil
      safe_s = @safe ? " #{ASTDump.key("safe")}" : ""
      io << ASTDump.nd("MemberAccess") << safe_s << " " << ASTDump.val("'#{@name}'") << ASTDump.loc(@loc) << "\n"
      field(io, "receiver", @receiver, prefix, true)
    end
  end

  class BinaryOp
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("BinaryOp") << " " << ASTDump.val("'#{ASTDump.op(@op)}'") << ASTDump.loc(@loc) << "\n"
      field(io, "lhs", @left,  prefix, false)
      field(io, "rhs", @right, prefix, true)
    end
  end

  class UnaryOp
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("UnaryOp") << " " << ASTDump.val("'#{ASTDump.op(@op)}'") << ASTDump.loc(@loc) << "\n"
      field(io, "operand", @operand, prefix, true)
    end
  end

  class Assign
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("Assign") << ASTDump.loc(@loc) << "\n"
      has_type = !@type_ann.nil?
      field(io, "target", @target, prefix, false)
      if ta = @type_ann
        field(io, "type", ta, prefix, false)
      end
      field(io, "value", @value, prefix, true)
    end
  end

  class VarDecl
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("VarDecl") << " " << ASTDump.val(@name) << ASTDump.loc(@loc) << "\n"
      field(io, "type", @type_ann, prefix, true)
    end
  end

  class BlockExpr
    def dump(io : IO, prefix : String) : Nil
      params_s = @params.empty? ? "" : " #{ASTDump.val("|#{@params.join(", ")}|")}"
      io << ASTDump.nd("BlockExpr") << params_s << ASTDump.loc(@loc) << "\n"
      section(io, "body", @body, prefix, true) unless @body.empty?
    end
  end

  class Call
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("Call") << ASTDump.loc(@loc) << "\n"
      has_args  = !@args.empty?
      has_block = !@block.nil?
      field(io, "callee", @callee, prefix, !has_args && !has_block)
      section(io, "args", @args.map(&.as(ANode)), prefix, !has_block) if has_args
      if b = @block
        field(io, "block", b, prefix, true)
      end
    end
  end

  class MethodCall
    def dump(io : IO, prefix : String) : Nil
      safe_s = @safe ? " #{ASTDump.key("safe")}" : ""
      io << ASTDump.nd("MethodCall") << safe_s << " " << ASTDump.val("'#{@name}'") << ASTDump.loc(@loc) << "\n"
      has_args  = !@args.empty?
      has_block = !@block.nil?
      field(io, "receiver", @receiver, prefix, !has_args && !has_block)
      section(io, "args", @args.map(&.as(ANode)), prefix, !has_block) if has_args
      if b = @block
        field(io, "block", b, prefix, true)
      end
    end
  end

  class Index
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("Index") << ASTDump.loc(@loc) << "\n"
      field(io, "receiver", @receiver, prefix, false)
      field(io, "index",    @index,    prefix, true)
    end
  end

  class PipeExpr
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("PipeExpr") << ASTDump.loc(@loc) << "\n"
      field(io, "lhs", @left,  prefix, false)
      field(io, "rhs", @right, prefix, true)
    end
  end

  class IfExpr
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("IfExpr") << ASTDump.loc(@loc) << "\n"
      has_els  = !@elsifs.empty?
      has_else = !@else_b.nil?
      field(io, "cond", @cond, prefix, false)
      section(io, "then", @then_b, prefix, !has_els && !has_else)
      @elsifs.each_with_index do |(cond, body), i|
        is_last = i == @elsifs.size - 1 && !has_else
        io << prefix << arm(!is_last) << ASTDump.lbl("elsif") << "\n"
        sub = cpfx(prefix, is_last)
        io << sub << arm(false)
        cond.dump(io, cpfx(sub, false))
        section(io, "body", body, sub, true)
      end
      if eb = @else_b
        section(io, "else", eb, prefix, true)
      end
    end
  end

  class WhileExpr
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("WhileExpr") << ASTDump.loc(@loc) << "\n"
      field(io, "cond", @cond, prefix, false)
      section(io, "body", @body, prefix, true)
    end
  end

  class MatchArm
    def dump(io : IO, prefix : String) : Nil
      label = @is_else ? "MatchArm #{ASTDump.key("else")}" : ASTDump.nd("MatchArm")
      io << label << "\n"
      has_pats = !@patterns.empty?
      if has_pats
        io << prefix << ASTDump::BRANCH << ASTDump.lbl("patterns") << "\n"
        sub = prefix + ASTDump::VERT
        @patterns.each_with_index do |p, i|
          pl = i == @patterns.size - 1
          io << sub << (pl ? ASTDump::CORNER : ASTDump::BRANCH)
          p.dump(io, sub + (pl ? ASTDump::SPACE : ASTDump::VERT))
        end
      end
      io << prefix << ASTDump::CORNER << ASTDump.lbl("body") << "\n"
      sub = prefix + ASTDump::SPACE
      io << sub << ASTDump::CORNER
      @body.dump(io, sub + ASTDump::SPACE)
    end
  end

  class MatchExpr
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("MatchExpr") << ASTDump.loc(@loc) << "\n"
      field(io, "value", @value, prefix, false)
      io << prefix << arm(true) << ASTDump.lbl("arms") << "\n"
      sub = cpfx(prefix, true)
      @arms.each_with_index do |a, i|
        al = i == @arms.size - 1
        io << sub << (al ? ASTDump::CORNER : ASTDump::BRANCH)
        a.dump(io, sub + (al ? ASTDump::SPACE : ASTDump::VERT))
      end
    end
  end

  class RangeExpr
    def dump(io : IO, prefix : String) : Nil
      excl_s = @exclusive ? " #{ASTDump.key("exclusive")}" : ""
      io << ASTDump.nd("RangeExpr") << excl_s << ASTDump.loc(@loc) << "\n"
      field(io, "from", @from, prefix, false)
      field(io, "to",   @to,   prefix, true)
    end
  end

  class AwaitExpr
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("AwaitExpr") << ASTDump.loc(@loc) << "\n"
      field(io, "expr", @expr, prefix, true)
    end
  end

  class ReturnExpr
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("ReturnExpr") << ASTDump.loc(@loc) << "\n"
      if v = @value
        field(io, "value", v, prefix, true)
      end
    end
  end

  class BreakExpr
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("BreakExpr") << ASTDump.loc(@loc) << "\n"
      if v = @value
        field(io, "value", v, prefix, true)
      end
    end
  end

  class NextExpr
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("NextExpr") << ASTDump.loc(@loc) << "\n"
      if v = @value
        field(io, "value", v, prefix, true)
      end
    end
  end

  class RaiseExpr
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("RaiseExpr") << ASTDump.loc(@loc) << "\n"
      field(io, "value", @value, prefix, true)
    end
  end



  class SimpleType
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("SimpleType") << " " << ASTDump.val("'#{@name}'") << ASTDump.loc(@loc) << "\n"
    end
  end

  class GenericType
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("GenericType") << " " << ASTDump.val("'#{@name}'") << ASTDump.loc(@loc) << "\n"
      unless @params.empty?
        io << prefix << ASTDump::CORNER << ASTDump.lbl("params") << "\n"
        sub = prefix + ASTDump::SPACE
        @params.each_with_index do |p, i|
          pl = i == @params.size - 1
          io << sub << (pl ? ASTDump::CORNER : ASTDump::BRANCH)
          p.dump(io, sub + (pl ? ASTDump::SPACE : ASTDump::VERT))
        end
      end
    end
  end

  class FuncType
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("FuncType") << ASTDump.loc(@loc) << "\n"
      unless @params.empty?
        io << prefix << ASTDump::BRANCH << ASTDump.lbl("params") << "\n"
        sub = prefix + ASTDump::VERT
        @params.each_with_index do |p, i|
          pl = i == @params.size - 1
          io << sub << (pl ? ASTDump::CORNER : ASTDump::BRANCH)
          p.dump(io, sub + (pl ? ASTDump::SPACE : ASTDump::VERT))
        end
      end
      field(io, "return_type", @return_type, prefix, true)
    end
  end

  class NilableType
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("NilableType") << ASTDump.loc(@loc) << "\n"
      field(io, "inner", @inner, prefix, true)
    end
  end


  class PointerType
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("PointerType") << ASTDump.loc(@loc) << "\n"
      field(io, "inner", @inner, prefix, true)
    end
  end


  class ArrayType
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("ArrayType") << " " << ASTDump.val("[#{@size}]") << ASTDump.loc(@loc) << "\n"
      field(io, "elem", @elem, prefix, true)
    end
  end


  class TypeofExpr
    def dump(io : IO, prefix : String) : Nil
      io << ASTDump.nd("TypeofExpr") << ASTDump.loc(@loc) << "\n"
      field(io, "operand", @operand, prefix, true)
    end
  end


  class SizeofExpr
      def dump(io : IO, prefix : String) : Nil
        io << ASTDump.nd("SizeofExpr") << ASTDump.loc(@loc) << "\n"
        field(io, "type", @type_node, prefix, true)
      end
    end


end
