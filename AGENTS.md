# AGENTS.md

notes for coding agents (and humans) working on this repository. short version:
it's one C++ file, don't overthink it.

## what this is

`simpledir` is directory shortcuts for the shell. an alias is a name you choose
mapped to an absolute path in `~/.simpledir/config.json`; `sd <name>` jumps
there. it also keeps a frecency log, so `sd <word>` finds directories you never
named — that part is a zoxide replacement.

**two commands, one binary.** the program looks at `argv[0]` and picks its half:
`sd` moves you and only reads the config, `sdcfg` changes things and never moves
you. keep it that way — the split is the whole ergonomic argument, and it means
`sd` is safe in a pipeline while `sdcfg` is safe to hit by reflex.

## ground rules

0. **no AI, ever.** no model inference, no generated suggestions, no API keys, no
   "ask an assistant" anything. every feature here is arithmetic over data already
   on this machine: frecency is a half-life, `suggest` counts `cd` targets in your
   history, typo suggestions are substring and edit distance. v8's "adaptive" is
   statistics plus an explicit accept or reject, not a guess from a model. if a
   feature can't be built without intelligence, it doesn't get built. this is the
   reason the tool has two dependencies and works on a plane.
1. **one source file on purpose.** `sd.cpp` is the entire program. resist
   splitting it, adding a build system beyond the one line in the Makefile, or
   pulling in a JSON or CLI library. libstdc++ and curl are the whole dependency
   list.
2. **the shell does the `cd`.** a subprocess cannot change the calling shell's
   cwd. `sd print` writes a path to stdout; the function emitted by `sdcfg init`
   performs the `cd`. never make a subcommand try to `cd`. only `sd` needs the
   function; `sdcfg` must stay a plain executable so nothing can shadow it.
3. **functions, not shell aliases.** bash expands aliases only in interactive
   shells, so an alias-based wrapper breaks in scripts and `eval`. keep the
   wrapper a function.
4. **writes are atomic.** temp file plus `os.replace`/rename, same for the
   config, the history log, and the binary that `update` swaps in.
5. **no silent failure.** an unknown alias, a missing target directory or a
   corrupt config must produce an explanatory message and a non-zero exit.
   `sd ls` tags dead paths `[missing]`.
6. **`UserError` vs `UsageError`.** the first exits 1, the second exits 2, which
   is what every other cli does for "you called it wrong". don't collapse them.
7. **keep `ls` machine-readable.** `--names` is one alias per line and `--json`
   emits the config file's shape. completion scripts and anything scripting this
   tool depend on those formats; don't reformat them casually.
8. **no network except `update`, `revert`, `releases` and the nudge.** `sd print`
   must never touch the network, and every network call must swallow its own
   errors — an update check is not worth breaking somebody's `cd`. the nudge only
   prints when stderr is a tty. TLS isn't in the C++ stdlib, so downloads go
   through `popen("curl …")`; tests point that at `file://` stubs so the suite
   never hits the network.
9. **never trust a download.** `update` and `revert` run the fetched file with
   `--version` and check both the program name and the version before replacing
   anything. keep that check.
10. **`SIMPLEDIR_RC` is exclusive.** when set, `uninstall` and `doctor` touch
    only that file. never widen it to the guessed candidates — a test run once
    removed the wrapper from a real `~/.bashrc` because of exactly that.
11. **read paths never resolve symlinks.** `as_stored()` is the only thing you
    call when turning a config value into a path; `realpath()` (which resolves)
    is for `add`/`import` only. resolving on read silently undoes
    `--keep-symlinks`.
12. **the split is not decorative.** when you add a verb, decide which half owns
    it: something that only reads belongs in `sd`, something that writes or
    maintains belongs in `sdcfg`. a verb that both reads and writes is a sign it
    wants splitting in two — that is how `suggest --bind` became `sd suggest`
    plus `sdcfg bind`.
13. **`migrate` is the config, `update` is the program.** `sdcfg migrate` moves
    the config file v1 → v2; `sdcfg update` replaces the binary. one character
    apart and completely unrelated, so never let one call the other, and never
    word an error message so the two could be confused.
14. **the shell wrapper must survive being pasted.** `eval $(sdcfg init)` word
    splits the output, folds every newline away and pathname-expands every
    unquoted `*`. so the generated block ends each statement with `;`, uses
    `[ "${1:0:1}" = "/" ]` instead of a `case` glob, and puts its comments
    *after* the last function. `make install` wires the rc **after** installing
    the binary, since `sdcfg init` is what generates the block.
15. **the prompt segment is state, and the state lives in the shell.**
    `install_release` isn't the only thing that needs a `cd`: `SD_ALIAS` and
    `SD_JUMPED_TO` are exported by the wrapper, because a subprocess cannot set
    them either. the prompt checks `$PWD` against `SD_JUMPED_TO` rather than
    hooking `cd`, so a plain `cd` clears the marker for free. never wrap `cd`.
    `sdcfg prompt bash` rebuilds PS1 from a saved base every time — appending to
    PS1 in a hook stacks a new copy on every prompt.
16. **history files are logs, not a data format.** `sd suggest` reads three
    different formats from three different shells and every one of them will
    contain junk. parse defensively, never throw, and skip what isn't a directory
    that exists. `HOME` in a test must point somewhere empty or the developer's
    real history leaks into the counts.

## before you touch anything

```bash
make test     # 391 assertions, spawns real bash to verify the wrapper
```

it must be 391/391 (or more) before you commit. the suite drives the *compiled*
binary through the same command-line surface a user does, and it covers the
python-era behaviours too: the `cd` the wrapper actually performs, `install.sh`
(platform refusal, asset download, compile fallback, install/uninstall round
trip) and the whole update path against `file://` stubs.

`tests/run.sh` uses a pty via `script -qec` for the two update-nudge checks and
the installer's repair/uninstall prompt, because those deliberately do nothing
when stdin isn't a terminal.

## layout

| path | what it is |
| --- | --- |
| `sd.cpp` | the entire program: json, config, frecency, both command halves |
| `sd` | build output. gitignored. `make` |
| `install.sh` | the `curl \| bash` installer. detects the platform, downloads a binary or compiles one, wires the rc |
| `tests/run.sh` | the test suite. plain bash, no framework, temp config dir |
| `docs/demo.tape` | [vhs](https://github.com/charmbracelet/vhs) script for `docs/demo.gif` |
| `packaging/PKGBUILD` | Arch package. bump `pkgver` on release |
| `packaging/nix/` | a nixpkgs expression |
| `Makefile` | `binaries` / `install` / `uninstall` / `test` / `assets` / `release` |

## conventions

- **long flags only.** `--force`, `--yes`, `--top`, `--to`. no short aliases.
  `parse_args()` takes a set of value-taking flags (`depth`, `prefix`, `top`,
  `to`, `path`); everything else is a boolean switch.
- error messages: lowercase, no trailing period on the first line, and always
  name the command that fixes it (`sd ls`, `sdcfg add --force ...`).
- paths: expand `~` and `$VARS`, then resolve. do this once at `add` time. on
  read use `as_stored()`, never `realpath()` — see rule 11.
- stdlib only. no runtime dependency beyond libstdc++ and curl.
- when you add a verb: the `COMMANDS` set in the shell wrapper's `case`, the
  dispatch in `run_move`/`run_config`, and a test.
- tests: add an assertion to `tests/run.sh` for every behaviour change. use the
  `check` (exit code), `contains`, `lacks` and `has` helpers. `$SD` is the move
  half and `$CFG` the config half, both the same binary.

## frecency

three constants at the top of `sd.cpp` and nothing else:

- `HALF_LIFE` (3 days) — a visit halves in worth every three days, computed
  lazily at read time, so nothing has to run in the background
- `VISIT_THROTTLE` (60s) — one write per directory per minute, so holding down a
  key in a shell doesn't rewrite the file fifty times
- `FLOOR` / `MAX_HISTORY` — forget what has faded, keep at most 500 entries

matching skips directories that have a name, because `sd name` is the way to
reach those. recording doesn't skip them, because `sd top` should show what you
actually do.

## recording the demo

```bash
vhs docs/demo.tape
```

needs `vhs` **and** `ttyd` on `PATH`. sleeps after `Enter` in the tape are
load-bearing — ttyd needs a beat to swallow the newline or the next `Type`
glues onto the same command line. the tape uses `SIMPLEDIR_CONFIG_DIR` so a
recording never touches a real `~/.simpledir`, and `SIMPLEDIR_NO_UPDATE_CHECK`
so a check can't phone home mid-take.

**the tape carries no setup commands.** `Hide` doesn't help: it hides the
keystrokes but the shell still echoes them, so `export PATH` and `eval` sit on
screen in the finished GIF anyway. install the tool first (`make install`) and
let the recording start on `sdcfg --version`. if the wrapper stops working in a
fresh shell, the rc block is empty — check `make install`'s ordering, it used to
wire the rc before installing the binary that generates the wrapper.

## releases

one feature set per release, so the notes are a list of what a user can now do.

```bash
make test
make audit-tags     # every tag must match the version its commit declares
make release        # test + audit-tags + assets, then tags, pushes, publishes
```

**`audit-tags` is not optional.** the v1.0.0 tag pointed one commit late, at the
bump to 2.0.0, so `sdcfg update --to v1.0.0` installed a program that called
itself 2.0.0. it survived four releases because every test stubbed the thing that
would have noticed, and nothing compared a tag to the version string inside the
commit it points at. tag the commit that *declares* the version, and check.

or by hand:

```bash
git tag -a v6.1.0 -m "v6.1.0"
git push origin v6.1.0
gh release create v6.1.0 --title "v6.1.0" --notes "..." dist/sd-linux-x86_64 dist/sd.cpp dist/install.sh
```

- **stable release** (`v6.1.0`) — a feature that works and is tested. ship these
  freely; `sdcfg update` only ever moves you to the newest stable.
- **prerelease** (`v6.2.0-rc.1`, `--prerelease`) — for a feature you're still
  changing the shape of. `update`'s version comparison only accepts `x.y.z` tags,
  so nobody gets yanked onto half-finished work.
- keep `VERSION` in `sd.cpp`, the tag, and the release title in sync — and the
  tag must point at a commit whose own `VERSION` already says that number, not at
  the commit after it. `make audit-tags` checks all of them at once.
- if a tag really is wrong, retag it (`git tag -f -a vX.Y.Z <commit>`) rather than
  editing the version string: a published tag that lies is worse than a moved one,
  and moving it back to an ancestor loses no history.
- the release title starts with the version, then a short hook: `v6.1.0 — one
  curl and you're done`.
- assets are `sd-linux-<arch>` (the binary), `sd.cpp` and `install.sh`, all mode
  755. the installer looks for `sd-linux-x86_64` or `sd-linux-arm64` under
  `releases/latest/download/` and compiles from source when it finds neither.
