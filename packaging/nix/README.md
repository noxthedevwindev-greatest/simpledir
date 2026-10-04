# nixpkgs packaging for simpledir
#
# to submit: create a PR from your fork of nixpkgs, or let the bot do it.
# the bot is the easy way and nobody has to review your code:
#
#   1. put these two files in a repo (this one is fine)
#   2. add a workflow that runs nixpkgs-review, OR just use the web app:
#      https://github.com/Mic92/nixpkgs-review#web-interface
#      -> paste the repo + branch, it opens the PR for you
#
# it needs a `meta.mainProgram`, which is what makes `nix run` work, and
# wrapProgram because the tool prints a shell snippet that has to be eval'd:
# after `nix profile install simpledir` add
#
#   eval "$(simpledir init)"
#
# to your shell rc. that is opt-in on purpose: a package install should never
# edit your rc file behind your back.

{ lib
, buildPythonApplication
, python3
}:

buildPythonApplication rec {
  pname = "simpledir";
  version = "4.0.0";

  src = fetchFromGitHub {
    owner = "noxthedevwindev-greatest";
    repo = "simpledir";
    rev = "v${version}";
    hash = "sha256-0000000000000000000000000000000000000000000000000000=";
  };

  # pure python, no dependencies, no build step
  format = "pyproject";
  nativeBuildInputs = [ python3 ];

  # the whole program is one executable file
  installPhase = ''
    runHook preInstall
    install -Dm755 simpledir $out/bin/simpledir
    install -Dm644 README.md $out/share/doc/simpledir/README.md
    install -Dm644 LICENSE $out/share/doc/simpledir/LICENSE
    runHook postInstall
  '';

  # there is nothing to import as a library, so `pyproject`'s default checks
  # have nothing to do. the test suite is bash.
  doCheck = false;

  meta = with lib; {
    description = "Named directory shortcuts for the shell - a database-free zoxide alternative";
    longDescription = ''
      Bind names you choose to directories and jump to them with `sd <name>`.
      State is a single JSON file; there is no frecency database, no daemon
      and no dependencies beyond the Python standard library.
    '';
    homepage = "https://github.com/noxthedevwindev-greatest/simpledir";
    license = licenses.mit;
    mainProgram = "simpledir";
    platforms = platforms.unix;
  };
}