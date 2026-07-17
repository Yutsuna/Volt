{
  pkgs,
  ...
}:
let
  claude-plugins-src = pkgs.fetchFromGitHub {
    owner = "anthropics";
    repo = "claude-plugins-official";
    rev = "main";
    hash = "sha256-ZLfdy+Z6iwQv+PzTEIqwoFcKUd0M936y3D5SD/5szS8=";
  };
  clangd-lsp-plugin = pkgs.stdenv.mkDerivation {
    name = "claude-clangd-lsp";
    src = claude-plugins-src;
    installPhase = ''
      mkdir -p $out
      cp -r plugins/clangd-lsp/* $out/
    '';
  };
  claudeConf = ''
    ## graphify

    This project has a graphify knowledge graph at graphify-out/.

    Rules:
    - Before answering architecture or codebase questions, read graphify-out/GRAPH_REPORT.md for god nodes and community structure
    - If graphify-out/wiki/index.md exists, navigate it instead of reading raw files
    - After modifying code files in this session, run `graphify update .` to keep the graph current (AST-only, no API cost)
  '';
  claudeSettings = {
    hooks = {
      PreToolUse = [
        {
          matcher = "Glob|Grep";
          hooks = [
            {
              type = "command";
              command = "[ -f graphify-out/graph.json ] && echo '{\"hookSpecificOutput\":{\"hookEventName\":\"PreToolUse\",\"additionalContext\":\"graphify: Knowledge graph exists. Read graphify-out/GRAPH_REPORT.md for god nodes and community structure before searching raw files.\"}}' || true";
            }
          ];
        }
      ];
    };
    enabledPlugins = {
      "clangd-lsp" = true;
    };
  };
  settingsJson = builtins.toJSON claudeSettings;
in
{
  inherit clangd-lsp-plugin;

  shellHook = ''
        export ENABLE_LSP_TOOL="1"
        export CLAUDE_CODE_PLUGIN_SEED_DIR="${clangd-lsp-plugin}"

        (
           mkdir -p .claude

           cat << 'EOF' > .claude/settings.local.json
    ${settingsJson}
    EOF

           cat << 'EOF' > CLAUDE.md
    ${claudeConf}
    EOF
         ) >/dev/null 2>&1 </dev/null &
  '';

}
