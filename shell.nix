{ pkgs ? import <nixpkgs> {} }:

pkgs.mkShell {
  buildInputs = with pkgs; [
    uv
    python3
    # Raylib dependencies (GLFW + X11)
    glfw
    libGL
    xorg.libX11
    xorg.libXrandr
    xorg.libXi
    xorg.libXinerama
    xorg.libXcursor
    alsa-lib          # for audio (optional)
    pulseaudio        # optional, but avoids warnings
  ];

  # Some Python packages (like raylib) need to find the libraries at runtime.
  # NixOS uses rpath, but setting LD_LIBRARY_PATH can help for development.
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
      pkgs.parallel
    ]}:$LD_LIBRARY_PATH
  '';
}
