{
  mkShell,
  taplo,
  voltExtension,
  pkg-config,
  openssl,
  cargo,
  rustc,
  git,
  cmake, # <-- Étape 1 : Ajoutez l'argument cmake ici
  writeShellScriptBin,
  ...
}:
let
  setupZedCli = writeShellScriptBin "setup-zed-cli" ''
    set -e
    export PKG_CONFIG_PATH="${openssl.dev}/lib/pkg-config"

    mkdir -p .direnv/bin

    echo "Compiling extension_cli from Zed's repository..."

    CARGO_TARGET_DIR=.direnv/cargo-target cargo install \
      --root .direnv \
      --git https://github.com/zed-industries/zed \
      extension_cli

    echo -e "\n'extension_cli' has been successfully compiled into .direnv/bin!"
  '';
in
mkShell {
  name = "zed-volt-dev-shell";

  inputsFrom = [ voltExtension ];

  buildInputs = [
    taplo
    pkg-config
    openssl
    cargo
    rustc
    git
    cmake # <-- Étape 2 : Ajoutez le paquet cmake dans buildInputs
    setupZedCli
  ];

  shellHook = ''
    export PKG_CONFIG_PATH="${openssl.dev}/lib/pkg-config"

    mkdir -p .direnv/bin
    export PATH="$PWD/.direnv/bin:$PATH"

    if ! command -v extension_cli &> /dev/null; then
        cat << EOF
Zed's 'extension_cli' tool is not yet installed in your project.
Run the following command to install it interactively:
setup-zed-cli
        EOF
    fi
  '';
}
