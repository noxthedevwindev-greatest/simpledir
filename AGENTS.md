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

1. **one file on purpose.** the whole tool is `simpledir`, an executable Python
   file with no dependencies beyond the 3.8 stdlib. resist adding modules,
   packages, `pyproject.toml`, type stubs or a CLI framework. a
   `pip install` is not the install story; `make install` and a curl of one file
   is.
2. **the shell does the `cd`.** a subprocess cannot change the calling shell's
   cwd. `simpledir jump` prints a path and exits; the function emitted by
   `simpledir init` performs the `cd`. never make a subcommand try to `cd`.
3. **functions, not shell aliases.** bash expands aliases only in interactive
   shells, so an alias-based wrapper breaks in scripts and `eval`. keep the
   wrapper a function.
4. **config writes are atomic.** temp file in the same directory plus
   `os.replace`. keep it that way.
5. **no silent failure.** an unknown alias, a missing target directory or a
   corrupt config must produce an explanatory message and a non-zero exit.
   `ls` tags dead paths `[missing]`.
6. **keep `ls` machine-readable.** `--names` is one alias per line and `--json`
   emits the config file's shape. completion scripts and anything scripting this
   tool depend on those formats; don't reformat them casually.

## before you touch anything

```bash
make test     # 77 assertions, spawns real bash to verify the wrapper
```

it must be 77/77 (or more) before you commit. the suite covers the python
side, the JSON config, and the actual `cd` behavior of the emitted shell
function, so a change to `cmd_init` that looks cosmetic can still break a test.

## layout

| path | what it is |
| --- | --- |
| `simpledir` | the entire tool: config IO, subcommands, the wrapper generator |
| `tests/run.sh` | the test suite. plain bash, no framework, temp config dir |
| `docs/demo.tape` | [vhs](https://github.com/charmbracelet/vhs) script for `docs/demo.gif` |
| `packaging/PKGBUILD` | Arch package. bump `pkgver` on release |
| `Makefile` | `install` (binary + guarded bashrc block), `uninstall`, `test` |

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

```bash
git tag -a v1.1.0 -m "..."
git push origin v1.1.0
gh release create v1.1.0 --title "..." --notes "..."
```

keep the tag, the release title and the version in `simpledir` in sync. the
version lives in one place: `VERSION`.
