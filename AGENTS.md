# AGENTS.md

notes for coding agents (and humans) working on this repository. short version:
it's one Python file, don't overthink it.

## what this is

`simpledir` is named directory shortcuts. an alias is a name you choose mapped
to an absolute path in `~/.simpledir/config.json`. `sd <name>` jumps there.

the next zoxide, in the sense that it drops the frecency scoring and keeps the
jumping. it is not a zoxide replacement and the README says so — don't rewrite
the positioning.

## ground rules

1. **one file on purpose.** the runtime is `simpledir`, an executable Python
   file with no dependencies beyond the 3.8 stdlib. resist adding modules,
   packages, `pyproject.toml`, type stubs or a CLI framework. a
   `pip install` is not the install story; `curl | bash` and a single file are.
2. **the shell does the `cd`.** a subprocess cannot change the calling shell's
   cwd. `simpledir jump` prints a path and exits; the function emitted by
   `simpledir init` performs the `cd`. never make a subcommand try to `cd`.
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
7. **no network except `update` and the nudge.** `jump` must never touch the
   network, and every network call must swallow its own errors — an update check
   is not worth breaking somebody's `cd`. the nudge only prints when stderr is a
   tty. tests override `SIMPLEDIR_UPDATE_URL` / `SIMPLEDIR_UPDATE_ASSET_URL` with
   `file://` stubs so the suite never hits the network.
8. **never trust a download.** `update` runs the fetched asset with `--version`
   and checks both the program name and that the version is actually newer before
   it replaces anything. keep that check.

## before you touch anything

```bash
make test     # 143 assertions, spawns real bash to verify the wrapper
```

it must be 143/143 (or more) before you commit. the suite covers the python
side, the JSON config, the actual `cd` behavior of the emitted shell function,
`install.sh` (package-manager selection, install/uninstall round trip) and the
whole `update` path against `file://` stubs, so a change to `cmd_init` that looks
cosmetic can still break a test.

`tests/run.sh` uses a pty via `script -qec` for the two update-nudge checks,
because the nudge is deliberately suppressed when stderr isn't a terminal.

## layout

| path | what it is |
| --- | --- |
| `simpledir` | the entire tool: config IO, subcommands, the wrapper generator, update logic |
| `install.sh` | the `curl \| bash` installer. finds python (yay → pacman → mise), installs, wires the rc |
| `tests/run.sh` | test suite. plain bash, no framework, temp config dir |
| `docs/demo.tape` | [vhs](https://github.com/charmbracelet/vhs) script for `docs/demo.gif` |
| `packaging/PKGBUILD` | Arch package. bump `pkgver` on release |
| `Makefile` | `install` (binary + guarded rc block), `uninstall`, `test`, `assets`, `release` |

## conventions

- stdlib only. no runtime dependency, ever.
- `argparse` for the CLI. subcommands are registered in `COMMANDS`; the wrapper's
  passthrough list in `cmd_init` is derived from that tuple, so add new
  subcommands to `COMMANDS` and to the `sub.add_parser` block and the shell
  wrapper follows automatically. don't hand-maintain that case statement.
- error messages: lowercase, no trailing period on the first line, and always
  include the command that fixes it (`simpledir ls`, `simpledir add -f ...`).
- paths: expand `~` and `$VARS`, then `Path.resolve()`. do this once at `add`
  time and on read.
- tests: add an assertion to `tests/run.sh` for every behavior change. use the
  existing `check` (exit code) and `contains` (substring) helpers.

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
gh release create v3.1.0 --title "v3.1.0" --notes "..." simpledir install.sh
```

- **stable release** (`v3.1.0`) — a feature that works and is tested. ship these
  freely; `simpledir update` only ever moves you to the newest stable.
- **prerelease** (`v3.2.0-rc.1`, `--prerelease`) — for a feature you're still
  changing the shape of. prereleases are ignored by `update`'s version
  comparison, which only accepts `x.y.z` tags, so nobody gets yanked onto
  half-finished work. use them to get design feedback in the open.
- keep `VERSION` in `simpledir`, the tag, and the release title in sync.
- the release title starts with the version, then a short hook: `v3.1.0 — one
  curl and you're done`.
- assets are always `simpledir` and `install.sh`, mode 755. GitHub drops the
  executable bit on download, so `update` and `install.sh` both `chmod` what
  they fetch.
