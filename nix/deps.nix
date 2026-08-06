{
  pkgs,
}:
let
  llvmAttrs = pkgs.llvmPackages_latest;
in
{
  nativeBuildInputs = with pkgs; [
    meson
    ninja
    pkg-config
    ccache
    mold
  ];

  buildInputs = with llvmAttrs; [
    llvm
    libllvm
  ];
}
