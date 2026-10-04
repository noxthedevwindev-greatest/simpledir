# AGENTS.md

notes for coding agents (and humans) working on this repository. short version:
it's one Python file, don't overthink it.

## what this is

`simpledir` is named directory shortcuts. an alias is a name you choose mapped
to an absolute path in `~/.simpledir/config.json`. `sd <name>` jumps there.

**two commands, one file.** the program looks at `argv[0]` and picks its half:
`sd` moves you and only reads the config, `sdcfg` changes things and never moves
you. keep it that way — the split is the whole ergonomic argument, and it means
`sd` is safe in a pipeline while `sdcfg` is safe to hit by reflex.

the next zoxide, in the sense that it drops the frecency scoring and keeps the
jumping. it is not a zoxide replacement and the README says so — don't rewrite
the positioning.

## ground rules

1. **one file on purpose.** the runtime is `sd`, an executable Python file with
   no dependencies beyond the 3.8 stdlib. `sdcfg` is a symlink to it; never make
   a second copy. resist adding modules,
   packages, `pyproject.toml`, type stubs or a CLI framework. a
   `pip install` is not the install story; `curl | bash` and a single file are.
2. **the shell does the `cd`.** a subprocess cannot change the calling shell's
   cwd. `sd print` prints a path and exits; the function emitted by `sdcfg init`
   performs the `cd`. never make a subcommand try to `cd`. only `sd` needs the
   function; `sdcfg` must stay a plain executable so nothing can shadow it.
3. **functions, not shell aliases.** bash expands aliases only in interactive
   shells, so an alias-based wrapper breaks in scripts and `eval`. keep the
   wrapper a function.
4. **config writes are atomic.** temp file in the same directory plus
   `os.replace`. keep it that way. `update` swaps binaries the same way.
5. **no silent failure.** an unknown alias, a missing target directory or a
   corrupt config must produce an explanatory message and a non-zero exit.
   `ls` tags dead paths `[missing]`.
6. **keep `ls` machine-readable.** `--names` is one alias per line and `--json`
   emits the config file's shape. completion scripts and anything scripting this
   tool depend on those formats; don't reformat them casually.
7. **no network except `update` and the nudge.** `sd print` must never touch the
   network, and every network call must swallow its own errors — an update check
   is not worth breaking somebody's `cd`. the nudge only prints when stderr is a
   tty. tests override `SIMPLEDIR_UPDATE_URL` / `SIMPLEDIR_UPDATE_ASSET_URL` with
   `file://` stubs so the suite never hits the network.
8. **never trust a download.** `update` runs the fetched asset with `--version`
   and checks both the program name and that the version is actually newer before
   it replaces anything. keep that check. `sdcfg update` is the only thing that
   writes `sd` outside of an install.
9. **`SIMPLEDIR_RC` is exclusive.** when set, `uninstall` and `doctor` touch
   only that file. never widen it to the guessed candidates — a test run once
   removed the wrapper from a real `~/.bashrc` because of exactly that.
10. **read paths never resolve symlinks.** `as_stored()` is the only thing you
    call when turning a config value into a path; `normalize()` (which resolves)
    is for `add`/`import` only. resolving on read silently undoes
    `--keep-symlinks`.
11. **the split is not decorative.** when you add a verb, decide which half owns
    it: something that only reads belongs in `sd`, something that writes or
    maintains belongs in `sdcfg`. a verb that both reads and writes is a sign it
    wants splitting in two — that is how `suggest --bind` became `sd suggest`
    plus `sdcfg bind`.
12. **history files are logs, not a data format.** `sd suggest` reads three
    different formats from three different shells and every one of them will
    contain junk. parse defensively, never raise, and skip what isn't a
    directory that exists. `HOME` in a test must point somewhere empty or the
    developer's real history leaks into the counts.

## before you touch anything

```bash
make test     # 235 assertions, spawns real bash to verify the wrapper
```

it must be 235/235 (or more) before you commit. the suite covers the python
side, the JSON config, the actual `cd` behavior of the emitted shell function,
`install.sh` (package-manager selection, both binaries, install/uninstall round
trip), the whole `update` path against `file://` stubs, and `suggest`/`i` against
fixtures and stub binaries, so the suite still never touches the network or your
real home directory.

the suite also asserts the split itself: that `sd` rejects config verbs and
points at `sdcfg`, that `sdcfg` rejects move verbs and points at `sd`, and that
only `sd`'s function changes directory.

`tests/run.sh` uses a pty via `script -qec` for the two update-nudge checks,
because the nudge is deliberately suppressed when stderr isn't a terminal.

## layout

| path | what it is |
| --- | --- |
| `sd` | the entire tool: config IO, both parsers, the wrapper generator, update logic |
| `install.sh` | the `curl \| bash` installer. finds python (yay → pacman → mise), installs, wires the rc |
| `tests/run.sh` | test suite. plain bash, no framework, temp config dir |
| `docs/demo.tape` | [vhs](https://github.com/charmbracelet/vhs) script for `docs/demo.gif` |
| `packaging/PKGBUILD` | Arch package. bump `pkgver` on release |
| `Makefile` | `install` (binary + guarded rc block), `uninstall`, `test`, `assets`, `release` |

## conventions

- stdlib only. no runtime dependency, ever.
- `argparse`, one parser per half: `build_move_parser()` and
  `build_config_parser()`. a new verb goes in `MOVE_VERBS` or `CONFIG_VERBS`
  (whichever owns it), in its `sub.add_parser` block, and in the `case` arm the
  other half prints when someone reaches for it in the wrong place.
- **flags are long-form only.** no short aliases. one less thing to misremember,
  and it is what the docs promise.
- error messages: lowercase, no trailing period on the first line, and always
  include the command that fixes it (`sd ls`, `sdcfg add --force ...`).
- paths: expand `~` and `$VARS`, then `Path.resolve()`. do this once at `add`
  time. on read use `as_stored()`, never `normalize()` — see rule 10.
- tests: add an assertion to `tests/run.sh` for every behavior change. use the
  existing `check` (exit code), `contains`, `lacks` and `has` helpers. `$SD` is
  the move half and `$CFG` the config half, both pointing at the same file.
- `SD` and `CONFIG` constants in the source are the command names. never
  hardcode `sd` or `sdcfg` in a message or an f-string; use the constants.

## recording the demo

```bash
vhs docs/demo.tape
```

needs `vhs` **and** `ttyd` on `PATH`. sleeps after `Enter` in the tape are
load-bearing — ttyd needs a beat to swallow the newline or the next `Type`
glues onto the same command line. the tape uses `SIMPLEDIR_CONFIG_DIR` so a
recording never touches a real `~/.simpledir`.

## releases

one feature set per release, so the notes are a list of what a user can now do.
each is either a minor release or a prerelease:

```bash
make test
make release        # reads VERSION, tags, pushes, publishes, uploads dist/
```

or by hand:

```bash
git tag -a v3.1.0 -m "v3.1.0"
git push origin v3.1.0
gh release create v3.1.0 --title "v3.1.0" --notes "..." sd install.sh
```

- **stable release** (`v3.1.0`) — a feature that works and is tested. ship these
  freely; `sdcfg update` only ever moves you to the newest stable.
- **prerelease** (`v3.2.0-rc.1`, `--prerelease`) — for a feature you're still
  changing the shape of. prereleases are ignored by `update`'s version
  comparison, which only accepts `x.y.z` tags, so nobody gets yanked onto
  half-finished work. use them to get design feedback in the open.
- keep `VERSION` in `sd`, the tag, and the release title in sync.
- the release title starts with the version, then a short hook: `v3.1.0 — one
  curl and you're done`.
- assets are always `sd` and `install.sh`, mode 755. GitHub drops the
  executable bit on download, so `update` and `install.sh` both `chmod` what
  they fetch.
