{
  description = "a native C++ jupyter kernel for the nix language, powered by lix";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    lix.url = "git+https://git.lix.systems/lix-project/lix";
    lix.inputs.nixpkgs.follows = "nixpkgs";
    flake-utils.url = "github:numtide/flake-utils";
    nixpkgs-fmt.url = "github:nix-community/nixpkgs-fmt";
  };

  outputs = { self, nixpkgs, lix, flake-utils, nixpkgs-fmt }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        xeus-lix-pkg = pkgs.stdenv.mkDerivation rec {
          pname = "xeus-lix";
          version = "0.1.0";

          src = ./.;

          nativeBuildInputs = [
            pkgs.cmake
            pkgs.pkg-config
            pkgs.clang_17
          ];

          buildInputs = [
            # lix dependencies
            lix.packages.${system}.nix.dev
            lix.packages.${system}.nix.passthru.capnproto-lix
            pkgs.boost

            # xeus dependencies
            pkgs.xeus
            pkgs.xeus-zmq
            pkgs.cppzmq
            pkgs.zeromq
            pkgs.openssl
            pkgs.nlohmann_json
          ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
            pkgs.libuuid
          ];

          meta = {
            description = "a native C++ jupyter kernel for the nix language, powered by lix";
            homepage = "https://github.com/ptrpaws/xeus-lix";
            license = pkgs.lib.licenses.lgpl21Plus;
          };
        };

      in
      {
        packages.default = xeus-lix-pkg;

        devShells.default = pkgs.mkShell {
          name = "xeus-lix-dev-env";

          packages = [
            # for building the kernel
            xeus-lix-pkg

            # for running jupyter and tests
            pkgs.jupyter
            pkgs.python3
          ];

          shellHook = ''
            export JUPYTER_PATH="${xeus-lix-pkg}/share/jupyter"
            echo "JUPYTER_PATH is set to the package's install directory."
            echo "to use the kernel, run 'jupyter lab'."
          '';
        };

        formatter = pkgs.nixpkgs-fmt;

        apps.default = {
          type = "app";
          program = "${pkgs.jupyter}/bin/jupyter-lab";
        };
      });
}
