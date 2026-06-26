require "colorize"


module Volt::CLI


  #   error[S0007]: undefined variable `x`
  #    --> foo.vl:3:5
  #     |
  #   3 |   y = x + 1
  #     |       ^ not found in this scope
  #     |
  #     = help: did you mean `xs`?
  class DiagnosticRenderer

    #------------------------------------------------------------------------------------

    def initialize( @sources : Hash( String, String ) )
    end

    #------------------------------------------------------------------------------------

    def render( bag : Frontend::DiagnosticBag, io : IO = STDERR ) : Nil
      first = true
      bag.each do |diagnostic|
        io.puts unless first
        first = false
        render_one( diagnostic, io )
      end
    end

    #------------------------------------------------------------------------------------

    private def render_one( diagnostic : Frontend::Diagnostic, io : IO ) : Nil
      io.puts header( diagnostic )

      width = gutter_width( diagnostic )
      pad   = " " * width

      if primary = diagnostic.labels.find( &.primary )
        io.puts "#{pad} #{"-->".colorize( :blue ).bold} #{location( primary.span )}"
      end

      diagnostic.labels.each_with_index do |label, i|
        io.puts "#{pad} #{bar}" if i == 0
        render_label( label, io, width, severity_color( diagnostic.severity ) )
      end

      unless diagnostic.notes.empty?
        io.puts "#{pad} #{bar}"
        diagnostic.notes.each do |note|
          io.puts "#{pad} #{"=".colorize( :blue ).bold} #{"help:".colorize( :cyan ).bold} #{note}"
        end
      end
    end

    private def render_label( label : Frontend::Label, io : IO, width : Int32, color : Symbol ) : Nil
      span   = label.span
      pad    = " " * width
      lineno = span.line.to_s.rjust( width )

      if text = line_text( span )
        io.puts "#{lineno.colorize( :blue ).bold} #{bar} #{text}"
        indent  = " " * ( span.column - 1 )
        glyph   = label.primary ? "^" : "-"
        carets  = ( glyph * Math.max( span.length, 1_u32 ) ).colorize( color ).bold
        message = label.message.empty? ? "" : " #{label.message.colorize( color )}"
        io.puts "#{pad} #{bar} #{indent}#{carets}#{message}"
      else
        msg = label.message.empty? ? "" : " #{label.message}"
        io.puts "#{pad} #{bar} #{location( span )}#{msg}"
      end
    end

    #------------------------------------------------------------------------------------

    private def header( diagnostic : Frontend::Diagnostic ) : String
      color = severity_color( diagnostic.severity )
      tag   = "#{severity_word( diagnostic.severity )}[#{diagnostic.code}]".colorize( color ).bold
      "#{tag}#{":".colorize( color ).bold} #{diagnostic.message.colorize.bold}"
    end

    private def location( span : Frontend::Span ) : String
      "#{span.file}:#{span.line}:#{span.column}"
    end

    private def bar
      "|".colorize( :blue ).bold
    end

    private def gutter_width( diagnostic : Frontend::Diagnostic ) : Int32
      max = diagnostic.labels.map( &.span.line ).max?
      ( max || 1_u32 ).to_s.size
    end

    private def line_text( span : Frontend::Span ) : String?
      src = @sources[ span.file ]?
      return nil unless src
      lines = src.lines
      idx   = span.line.to_i - 1
      return nil unless 0 <= idx < lines.size
      lines[ idx ]
    end

    private def severity_word( severity : Frontend::Severity ) : String
      case severity
      when .error?   then "error"
      when .warning? then "warning"
      when .note?    then "note"
      else                "help"
      end
    end

    private def severity_color( severity : Frontend::Severity ) : Symbol
      case severity
      when .error?   then :red
      when .warning? then :yellow
      when .note?    then :blue
      else                :cyan
      end
    end

    #------------------------------------------------------------------------------------

  end


end
