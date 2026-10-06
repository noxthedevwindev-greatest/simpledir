{ lib
, staticStdenv
, fetchFromGitHub
, curl
, makeWrapper
}:

staticStdenv.mkDerivation (finalAttrs: {
  pname = "simpledir";
  version = "10.0.0";

  src = fetchFromGitHub {
    owner = "noxthedevwindev-greatest";
    repo = "simpledir";
    rev = "v${finalAttrs.version}";
    # fill this in from the nixpkgs-review output, or:
    #   nix-prefetch-url --unpack \
    #     https://github.com/noxthedevwindev-greatest/simpledir/archive/v${version}.tar.gz
    hash = "sha256-REPLACE_ME_WITH_THE_PREFETCH_HASH";
  };

  strictDeps = true;

  # the update check, `revert` and `sdcfg releases` all shell out to curl: the
  # C++ standard library has no TLS, and curl is already the install dependency.
  nativeBuildInputs = [ curl makeWrapper ];

  enableParallelBuilding = true;

  # One source file, one compiler invocation. Fully static, so the binary doesn't
  # inherit this nixpkgs' glibc into a runtime closure the size of a distro --
  # and because the dynamic loader costs ~650us before main() runs, which is more
  # than this program spends doing its job. staticStdenv supplies a static libc,
  # so the fallback the Makefile needs is unnecessary here.
  buildPhase = ''
    runHook preBuild
    $CXX -std=c++17 -O2 -static -ffunction-sections -fdata-sections \
      -Wl,--gc-sections -o sd sd.cpp
    runHook postBuild
  '';

  doCheck = true;

  checkPhase = ''
    runHook preCheck
    # the suite needs bash and a compiler (it builds its own stub binaries) and
    # points every network call at file:// stubs, so nothing leaves the sandbox
    ln -sf $PWD/sd sdcfg
    bash tests/run.sh
    runHook postCheck
  '';

  installPhase = ''
    runHook preInstall
    install -Dm755 sd $out/bin/sd
    ln -s sd $out/bin/sdcfg
    install -Dm644 sd.cpp $out/share/doc/simpledir/sd.cpp
    install -Dm644 README.md $out/share/doc/simpledir/README.md
    install -Dm644 llms.txt $out/share/doc/simpledir/llms.txt
    install -Dm644 install.sh $out/share/doc/simpledir/install.sh
    runHook postInstall
  '';

  # `sdcfg update`, `revert` and `releases` run curl through popen, because the
  # C++ standard library has no TLS. linking curl in would be silly for one
  # `cd`, so it stays a wrapped runtime dependency instead: without this the
  # update check silently does nothing on a NixOS box with no curl on PATH.
  # the wrapper execs with argv[0] preserved, so `sdcfg` still sees its own name
  # and picks the right half.
  postFixup = ''
    wrapProgram $out/bin/sd --prefix PATH : ${lib.makeBinPath [ curl ]}
  '';

  meta = with lib; {
    description = "Named directory shortcuts for your shell - a frecency-aware zoxide alternative";
    longDescription = ''
      Bind names you choose to directories and jump to them, and remember the
      ones you never named. Two commands from one binary: sd moves you and only
      reads your config, sdcfg changes things and never moves you.
    '';
    homepage = "https://github.com/noxthedevwindev-greatest/simpledir";
    license = licenses.mit;
    platforms = platforms.linux;
    mainProgram = "sd";
  };
})
