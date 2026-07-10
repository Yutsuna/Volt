module Volt::CLI


  record CompletionCandidate, label : String, kind : Symbol

  record CompletionContext, text : String, cursor : Int32, word : String, word_start : Int32

  record CompletionResult, candidates : Array(CompletionCandidate), replace_start : Int32, replace_end : Int32

  # (text, cursor) in, candidates + replacement span out — the same shape as LSP
  # textDocument/completion, so an LSP-backed provider can slot in later.
  # Providers are registered as a plain array (see REPLCommand#completion_engine):
  # macro-based registration is not worth it for this few providers, one of which
  # needs a constructor argument.
  abstract class ACompletionProvider

    abstract def complete( context : CompletionContext ) : Array(CompletionCandidate)

  end


end
