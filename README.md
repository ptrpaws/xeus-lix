# xeus-lix: a native jupyter kernel for the nix language

a friendlier way to run nix code in jupyter, powered by the [lix](https://lix.systems/) evaluator.

## features

*   **interactive nix**: run any nix expression and see what it does, instantly.
*   **rich display**: get pretty, colorized outputs for nix data types.
*   **repl commands**: use the commands you know and love, like `:load`, `:load-flake`, `:build`, `:add`, and `:help`.
*   **shell integration**: run shell commands straight from a cell by starting with `!`.
*   **code completion**: hit tab to complete variables, attributes, and builtins.
*   **inspection**: press shift+tab to get docs for builtins and functions.

## installation & usage

### development environment

you can hop into a development shell with everything you need to build and test the kernel:

```bash
nix develop
```

### building

once you're in the dev shell, you can build the kernel package:

```bash
nix build
```

the package will show up in the `./result` symlink.

### running

to fire up a jupyterlab session with the xeus-lix kernel ready to go, just run:

```bash
nix run
```

this will pop open jupyterlab in your browser. from there, you can make a new notebook and pick the "nix (lix)" kernel.

you can also start jupyterlab yourself from inside the development shell (`nix develop`):

```bash
jupyter lab
```

### example notebooks

this repo has an example notebook to help you get started:

*   `notebooks/Intro.ipynb`: a general intro to what the kernel can do.

## license

this project is licensed under the lgplv2.1 or later.
