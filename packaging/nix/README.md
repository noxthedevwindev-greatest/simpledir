# nixpkgs packaging for simpledir

`package.nix` is the real expression. `flake.nix` wraps it so you can
`nix run` / `nix develop` from a checkout without wiring anything up.

## what it packages

v6 is one C++ file and a `curl` dependency:

- `buildPhase` is a single `$CXX -std=c++17 -O2` over `sd.cpp`, statically
  linked against libstdc++ so the runtime closure doesn't drag in a whole gcc
- `sd` goes in `$out/bin/sd` and `sdcfg` is a **symlink** to it. the program
  picks which half it is from `argv[0]`, so there is one file and two names —
  don't install a second copy
- `curl` is wrapped onto `PATH` with `wrapProgram` rather than linked, because
  the standard library has no TLS and `sdcfg update` shells out to it. without
  the wrapper the update check silently does nothing on a NixOS box

## after installing

a package install must not edit your rc file, so the wrapper is opt-in:

```bash
nix profile install nixpkgs#simpledir   # or: nix run .
eval "$(sdcfg init)" >> ~/.bashrc       # once
```

`sdcfg init` prints a block that survives being `eval`'d — see rule 14 in the
top-level `AGENTS.md`; it is glob-free and semicolon-terminated on purpose.

## submitting it to nixpkgs

the bot is the easy way and nobody has to review your code:

1. put `package.nix` in a repo (this one is fine) on a branch
2. open https://github.com/Mic92/nixpkgs-review#web-interface and paste the
   repo + branch; it opens the PR for you

`meta.mainProgram = "sd"` is what makes `nix run` work — keep it.

## before you send it

the one thing that always blocks a first submission is the `hash` in
`fetchFromGitHub`. get it with:

```bash
nix-prefetch-url --unpack \
  https://github.com/noxthedevwindev-greatest/simpledir/archive/v6.0.0.tar.gz
```

or just read it out of the `nixpkgs-review` run and paste it in. a placeholder
hash fails the build, it doesn't warn.

`doCheck` is on and runs `tests/run.sh`, which compiles its own stub binaries
and points every network call at `file://` URLs, so the check phase neither
needs the network nor a `checkInputs` list beyond bash and a compiler.
