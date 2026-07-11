module Volt::CLI


  record CompletionCandidate, label : String, kind : Symbol

  record CompletionContext, text : String, cursor : Int32, word : String, word_start : Int32,
                             receiver : String? = nil

  record CompletionResult, candidates : Array(CompletionCandidate), replace_start : Int32, replace_end : Int32

  # (text, cursor) in, candidates + replacement span out — the same shape as LSP
  # textDocument/completion, so an LSP-backed provider can slot in later.
  # Providers are registered as a plain array (see REPLCommand#completion_engine):
  # macro-based registration is not worth it for this few providers, one of which
  # needs a constructor argument.
  abstract class ACompletionProvider

    abstract def complete( context : CompletionContext ) : Array(CompletionCandidate)

    # Method tables key overloads as `name/arity` and include the lifecycle
    # hooks. Suggestions show the bare name only — overload resolution is the
    # interpreter's job — and never offer `initialize`/`finalize`: force-calling
    # a ctor/dtor on a live value is unsafe.
    protected def suggestible_method_name( raw : String ) : String?
      name = raw.index( '/' ).try { |i| raw[ 0, i ] } || raw
      return nil if name == "initialize" || name == "finalize"
      name
    end

  end


end
