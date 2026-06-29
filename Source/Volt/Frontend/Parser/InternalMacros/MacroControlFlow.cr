module Volt::Frontend


  class MacroParser

    # Handle a `{% if %}` / `{% for %}` directive starting at `start`. Returns the body
    # index just past the matching `{% end %}`.
    private def emit_directive( io : String::Builder,
                                body : Array( Token ),
                                start : Int32,
                                to : Int32,
                                bindings : Hash( String, String ),
                                call_span : Span ) : Int32
      head_close = matching( body, start, to, TokenKind::RMacroExpr, "{%" )
      head       = body[ ( start + 1 )...head_close ]
      keyword    = head.first?

      raise ExpansionError.new( "empty macro directive", body[ start ].span ) unless keyword

      case keyword.kind
      when .if?, .unless?
        emit_if( io, body, head_close, to, head, bindings, call_span )
      when .for?
        emit_for( io, body, head_close, to, head, bindings, call_span )
      else
        raise ExpansionError.new( "unsupported macro directive `#{ keyword.value }`", keyword.span )
      end
    end


    private def emit_if( io : String::Builder,
                         body : Array( Token ),
                         head_close : Int32,
                         to : Int32,
                         head : Array( Token ),
                         bindings : Hash( String, String ),
                         call_span : Span ) : Int32
      negate    = head.first.kind.unless?
      condition = evaluate_condition( head[ 1.. ], bindings, call_span )
      condition = !condition if negate

      branches = collect_branches( body, head_close + 1, to )

      # Take the first satisfied branch — standard if/elsif/else semantics. Reaching a
      # later branch already implies every earlier one was false, so the check is local.
      branches.each do |branch|
        taken = case branch.kind
                in BranchKind::Then  then condition
                in BranchKind::Elsif then evaluate_condition( branch.head, bindings, call_span )
                in BranchKind::Else  then true
                end

        if taken
          emit_sequence( io, body, branch.from, branch.to, bindings, call_span )
          break
        end
      end

      branches.last.end_index
    end


    private def emit_for( io : String::Builder,
                          body : Array( Token ),
                          head_close : Int32,
                          to : Int32,
                          head : Array( Token ),
                          bindings : Hash( String, String ),
                          call_span : Span ) : Int32
      # head == [ for, <var>, in, <collection tokens…> ]
      raise ExpansionError.new( "malformed `{% for %}`", head.first.span ) if head.size < 4
      var_tok = head[ 1 ]
      raise ExpansionError.new( "expected loop variable in `{% for %}`", var_tok.span ) unless var_tok.kind.ident?
      raise ExpansionError.new( "expected `in` in `{% for %}`", head[ 2 ].span ) unless head[ 2 ].kind.in?

      items = collection_items( head[ 3.. ], bindings )

      block_from = head_close + 1
      block_to   = matching_end( body, block_from, to )

      items.each do |item|
        scoped = bindings.dup
        scoped[ var_tok.value ] = item
        emit_sequence( io, body, block_from, block_to, scoped, call_span )
      end

      # Resume just past the closing `{% end %}` (three tokens: `{%`, `end`, `%}`).
      matching( body, block_to, to, TokenKind::RMacroExpr, "{%" ) + 1
    end


    enum BranchKind
      Then
      Elsif
      Else

      def elsif? : Bool
        self == Elsif
      end
    end


    record Branch,
      kind : BranchKind,
      head : Array( Token ),
      from : Int32,
      to : Int32,
      end_index : Int32


    # Split an `{% if %}` block into its then/elsif/else segments up to `{% end %}`.
    private def collect_branches( body : Array( Token ), from : Int32, to : Int32 ) : Array( Branch )
      branches = [] of Branch
      kind      = BranchKind::Then
      head      = [] of Token
      seg_from  = from
      depth     = 0
      i         = from

      while i < to
        if directive_keyword?( body, i, to ) { |kw| nesting_opener?( kw ) }
          depth += 1
          i = skip_directive( body, i, to )
          next
        end

        if depth == 0 && directive_keyword?( body, i, to ) { |kw| kw.kind.elsif? || kw.kind.else? || kw.kind.end? }
          close = matching( body, i, to, TokenKind::RMacroExpr, "{%" )
          kw    = body[ i + 1 ]
          branches << Branch.new( kind, head, seg_from, i, close + 1 )

          if kw.kind.end?
            return branches
          elsif kw.kind.elsif?
            kind     = BranchKind::Elsif
            head     = body[ ( i + 2 )...close ]
            seg_from = close + 1
          else # else
            kind     = BranchKind::Else
            head     = [] of Token
            seg_from = close + 1
          end
          i = close + 1
          next
        end

        if depth > 0 && directive_keyword?( body, i, to ) { |kw| kw.kind.end? }
          depth -= 1
          i = skip_directive( body, i, to )
          next
        end

        i += 1
      end

      raise ExpansionError.new( "unterminated `{% if %}`", body[ from - 1 ].span )
    end


    # Find the `{% end %}` that closes a `{% for %}` / nested block, returning the index
    # of the `{%` that opens it.
    private def matching_end( body : Array( Token ), from : Int32, to : Int32 ) : Int32
      depth = 0
      i     = from
      while i < to
        if directive_keyword?( body, i, to ) { |kw| nesting_opener?( kw ) }
          depth += 1
          i = skip_directive( body, i, to )
        elsif directive_keyword?( body, i, to ) { |kw| kw.kind.end? }
          return i if depth == 0
          depth -= 1
          i = skip_directive( body, i, to )
        else
          i += 1
        end
      end
      raise ExpansionError.new( "unterminated macro block", body[ from - 1 ].span )
    end


    private def nesting_opener?( kw : Token ) : Bool
      kw.kind.if? || kw.kind.unless? || kw.kind.for?
    end


    # Yield the keyword token of a `{% <kw> … %}` directive at `i`; true when the block
    # matches the predicate. Non-directives yield nothing and return false.
    private def directive_keyword?( body : Array( Token ), i : Int32, to : Int32, & : Token -> Bool ) : Bool
      return false unless body[ i ].kind.l_macro_expr?
      kw = body[ i + 1 ]?
      return false unless kw
      yield kw
    end


    private def skip_directive( body : Array( Token ), i : Int32, to : Int32 ) : Int32
      matching( body, i, to, TokenKind::RMacroExpr, "{%" ) + 1
    end

  end


end
