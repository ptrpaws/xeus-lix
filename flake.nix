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

        jupyter-kernel-test-pkg = pkgs.python3.pkgs.buildPythonPackage rec {
          pname = "jupyter_kernel_test";
          version = "0.7.0";
          format = "pyproject";

          src = pkgs.fetchPypi {
            inherit pname version;
            sha256 = "078b6fe7f770dd164f9549bdd7a355663225a3ff9b0f7575ad546d27239ec609";
          };

          nativeBuildInputs = with pkgs.python3.pkgs; [ hatchling ];

          propagatedBuildInputs = with pkgs.python3.pkgs; [
            jupyter-client
            jsonschema
            ipykernel
          ];

          doCheck = false;
        };

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

          cmakeFlags = [
            "-DPython3_EXECUTABLE=${pkgs.python3}/bin/python3"
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
            jupyter-kernel-test-pkg
          ];

          shellHook = ''
            export JUPYTER_PATH="${xeus-lix-pkg}/share/jupyter"
            echo "JUPYTER_PATH is set to the package's install directory."
            echo "to run tests locally, you can now simply run 'python3 -m unittest discover test'."
            echo "to use the kernel, run 'jupyter lab'."
          '';
        };

        checks.default = pkgs.stdenv.mkDerivation {
          name = "xeus-lix-tests";
          src = ./.;

          nativeBuildInputs = [
            pkgs.cmake
            pkgs.pkg-config
            pkgs.clang_17
            pkgs.python3
            jupyter-kernel-test-pkg
          ];

          buildInputs = xeus-lix-pkg.buildInputs;

          checkPhase = ''
            runHook preCheck
            mkdir build
            cd build
            cmake .. -DPython3_EXECUTABLE=${pkgs.python3}/bin/python3
            make

            # set JUPYTER_PATH so the test suite can find our kernel spec
            export JUPYTER_PATH=$PWD/share/jupyter

            # run the tests
            ctest -V
            runHook postCheck
          '';

          doInstall = false;
        };

        formatter = pkgs.nixpkgs-fmt;

        apps.default = {
          type = "app";
          program = "${pkgs.jupyter}/bin/jupyter-lab";
        };
      });
}
