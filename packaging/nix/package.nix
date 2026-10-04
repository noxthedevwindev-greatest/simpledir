{ lib
, stdenvNoCC
, fetchFromGitHub
, makeWrapper
, python3
}:

stdenvNoCC.mkDerivation rec {
  pname = "simpledir";
  version = "4.0.0";

  src = fetchFromGitHub {
    owner = "noxthedevwindev-greatest";
    repo = "simpledir";
    rev = "v${version}";
    # nixpkgs-review fills this in for you, or:
    #   nix-prefetch-url --unpack \
    #     https://github.com/noxthedevwindev-greatest/simpledir/archive/v${version}.tar.gz
    hash = "sha256-REPLACE_ME_WITH_THE_PREFETCH_HASH";
  };

  nativeBuildInputs = [ makeWrapper ];

  # pure python, one executable file, nothing to compile
  dontBuild = true;

  installPhase = ''
    runHook preInstall
    install -Dm755 simpledir $out/bin/simpledir
    install -Dm644 README.md $out/share/doc/simpledir/README.md
    install -Dm644 LICENSE $out/share/doc/simpledir/LICENSE

    # the shebang is /usr/bin/env python3, so the store's python has to be on PATH
    wrapProgram $out/bin/simpledir \
      --prefix PATH : ${lib.makeBinPath [ python3 ]}
    runHook postInstall
  '';

  # the test suite is bash and needs a writable $HOME; not worth it in CI
  doCheck = false;

  meta = {
    description = "Named directory shortcuts for the shell, without the frecency database";
    longDescription = ''
      Bind names you choose to directories and jump to them with sd <name>.
      State is a single JSON file: no frecency scoring, no database, no
      daemon, and no dependencies beyond the Python standard library.
    '';
    homepage = "https://github.com/noxthedevwindev-greatest/simpledir";
    license = lib.licenses.mit;
    mainProgram = "simpledir";
    platforms = lib.platforms.unix;
  };
}