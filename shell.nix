{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  buildInputs = with pkgs; [
    # C compiler & build tools
    gcc
    clang
    llvm
    pkg-config

    # Libraries
    raylib          # graphics
    sqlite          # database
    libmicrohttpd   # HTTP server
    cjson           # JSON parsing
    uthash          # header‑only hash tables (uthash.h)

    # Raylib runtime dependencies (already pulled by raylib, but we add them for LD_LIBRARY_PATH)
    glfw
    libGL
    xorg.libX11
    xorg.libXrandr
    xorg.libXi
    xorg.libXinerama
    xorg.libXcursor
    alsa-lib
    pulseaudio
  ];

  shellHook = ''
    export LD_LIBRARY_PATH=${pkgs.lib.makeLibraryPath [
      pkgs.glfw
      pkgs.libGL
      pkgs.xorg.libX11
      pkgs.xorg.libXrandr
      pkgs.xorg.libXi
      pkgs.xorg.libXinerama
      pkgs.xorg.libXcursor
      pkgs.alsa-lib
      pkgs.pulseaudio
    ]}:$LD_LIBRARY_PATH
    echo "C development environment ready."
    echo "Compile with: gcc -o graphviz main.c -lraylib -lsqlite3 -lmicrohttpd -lcjson -lpthread -lm"
  '';
}
