#{
# REPL/meson.build
# `volt repl`, split so that only the terminal layer is allowed any I/O.
#
# Dependency order is the layering: ReplEval owns the compile-and-run session
# and knows nothing about presentation; ReplCore drives a session and classifies
# what the user typed, and is still pure. The modules that follow in later
# phases (ReplDoc, ReplSyntax, ReplQuery, ReplComplete) are pure as well, and
# ReplTui — the only module permitted to touch a terminal — sits on top of all
# of them.
#}

subdir('ReplEval')
subdir('ReplCore')
