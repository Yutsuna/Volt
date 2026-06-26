module Volt::Frontend


  class DiagnosticBag
    getter diagnostics : Array( Diagnostic )

    def initialize
      @diagnostics = [] of Diagnostic
    end

    def <<( diagnostic : Diagnostic ) : DiagnosticBag
      @diagnostics << diagnostic
      self
    end

    def errors? : Bool
      @diagnostics.any?( &.severity.error? )
    end

    def error_count : Int32
      @diagnostics.count( &.severity.error? )
    end

    def empty? : Bool
      @diagnostics.empty?
    end

    def each( & : Diagnostic -> )
      sorted.each { |d| yield d }
    end

    def sorted : Array( Diagnostic )
      @diagnostics.sort_by do |d|
        if span = d.primary_span
          { span.line, span.column }
        else
          { UInt32::MAX, UInt32::MAX }
        end
      end
    end
  end


end
