<p align="center">
  <img src="docs/demo.gif" width="820" alt="simpledir demo: bind a name, jump to it, typo suggestions">
</p>

<h1 align="center">simpledir</h1>

<p align="center">
  <b>the next zoxide</b>
</p>

<p align="center">
  named directory shortcuts. all of the jump, none of the frecency database.
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue?style=flat-square" alt="MIT License"></a>
  <img src="https://img.shields.io/badge/python-3.8%2B-blue?style=flat-square&logo=python&logoColor=white" alt="Python 3.8+">
  <img src="https://img.shields.io/badge/dependencies-zero-brightgreen?style=flat-square" alt="zero dependencies">
  <img src="https://img.shields.io/badge/shell-bash%20%7C%20zsh-informational?style=flat-square" alt="bash or zsh">
  <img src="https://img.shields.io/github/stars/noxthedevwindev-greatest/simpledir?style=flat-square" alt="stars">
</p>

---

**simpledir** is a command line tool that maps names you choose to directories on
disk, so you can `cd` to a bookmark with `sd <name>`. State is a single JSON file
at `~/.simpledir/config.json` mapping alias names to absolute paths. The jump is
performed by a shell function that `simpledir init` appends to your shell rc,
because a subprocess cannot change the calling shell's working directory. No
frecency scoring, no database, no daemon, no dependencies — Python 3.8 stdlib
only.

```bash
cd ~/projects/dotfiles && simpledir add dots    # bind this dir
sd dots                                         # jump
simpledir dots                                  # same thing, spelled out
sd dots/nix                                     # jump into a subdirectory
```

that's the whole idea. names you choose, no scoring algorithm, no database.

> also here: [`llms.txt`](llms.txt) — a plain-text summary for language models
> and tools that fetch docs. [`AGENTS.md`](AGENTS.md) — notes for coding agents
> contributing to this repo.

## why not zoxide

[zoxide](https://github.com/ajeetdsouza/zoxide) is a great tool and this is not
a replacement for it. the difference is what each one optimizes for:

| | zoxide | simpledir |
| --- | --- | --- |
| remembers dirs you never named | yes, frecency-scored | no |
| state to maintain | sqlite db + daemon + shell hooks | one json file |
| recall a path from 6 months ago | `z foo` and it probably knows | `simpledir ls` and you look |
| startup cost | shell hook + db queries | one `cat` |
| config you can read and hand-edit | no | yes |
| moving a dir | move it, forget the alias | `simpledir add -f name /new/path` |
| installing it | a package manager | `curl … \| bash` |

**use zoxide** if you wander — frecency scoring is genuinely good and
reimplementing it would double this tool's size and add a database to a project
whose whole pitch is "no database".

**use simpledir** if you `cd` to the same dozen places every day and want them
to have names. it's also the thing you reach for when you *know* the name and
zoxide would make you guess a substring of it.

**use both.** they don't conflict — different prefixes, and they can even share
a name because each keeps its own state.

## install

one command. it checks for python, installs it with **yay** (then pacman, then
mise) if it's missing, drops the tool in `~/.local/bin`, and wires your shell rc:

```bash
curl -fsSL https://raw.githubusercontent.com/noxthedevwindev-greatest/simpledir/main/install.sh | bash
```

prefer to look before you pipe? that's the right instinct:

```bash
curl -fsSL https://raw.githubusercontent.com/noxthedevwindev-greatest/simpledir/main/install.sh -o install.sh
less install.sh && bash install.sh
```

`bash install.sh --uninstall` removes the binary and the rc block (your aliases
in `~/.simpledir/config.json` stay). installer knobs: `SIMPLEDIR_BIN_DIR`,
`SIMPLEDIR_RC`, `SIMPLEDIR_PM`, `SIMPLEDIR_NO_RC=1`.

by hand, if you'd rather:

```bash
git clone https://github.com/noxthedevwindev-greatest/simpledir
cd simpledir && make install
```

no package manager, no dependencies, python 3.8+. a PKGBUILD is in
`packaging/`; the AUR package isn't claimed yet — PR welcome.

## usage

```bash
simpledir add [<name>] [<path>]  # bind a name. name defaults to the dir's own name, path to $PWD
simpledir rm <name>              # unbind
simpledir rename <old> <new>     # rename, keep the path
simpledir import <dir>           # bind every subdirectory of a tree at once
simpledir ls [<query>] [-l]      # list, optionally filtered by name or path substring
simpledir ls --names             # one alias per line (for completion scripts)
simpledir ls --json              # same shape as the config file, for scripts and agents
simpledir jump <alias[/sub]>     # print the path, don't cd
simpledir edit                   # open the config in $EDITOR
simpledir update                 # check for a newer release, install it if you want
simpledir doctor                 # check your install: dead aliases, rc wiring, PATH
simpledir completions bash|zsh   # print a completion script
simpledir init                   # print the shell wrapper
```

and in your shell, `sd` / `simpledir` with no subcommand:

```bash
sd dots        # cd to the bound dir
sd dots/nix    # cd into a subdirectory of it
sd /some/path  # or just a path, why not
sd             # -> $HOME
sd -           # -> previous dir
simpledir dots # identical to `sd dots`
```

bulk binding, for when you have a whole directory of projects:

```bash
$ simpledir import ~/Projects
bound 12: dotfiles hyprland nixdots projects simpledir ...
```

typo suggestions, because `sd dotfile` shouldn't just fail:

```
$ sd dotfile
simpledir: no alias named 'dotfile'
  did you mean: dotfiles
  see them all: simpledir ls
```

## staying up to date

```bash
simpledir update              # check, then ask before installing
simpledir update --check      # just tell me. exit 1 means an update exists (CI-friendly)
simpledir update --yes        # don't ask
```

`update` downloads the release asset, checks that it's actually simpledir and
actually newer, then replaces your binary atomically. it won't overwrite a
working install with a same-version or garbage download.

it also checks on its own, at most **once a day**, and only when you're sitting
at a terminal — never on the jump path, never in a script, never if stderr isn't
a tty. all it does is print one line:

```
simpledir: v3.1.0 is out (you're on v3.0.0). `simpledir update` installs it.
```

opt out entirely with `SIMPLEDIR_NO_UPDATE_CHECK=1`.

## what's new in 3.0

**one-command install.** `curl … | bash`. it finds python, or installs it with
yay / pacman / mise, then installs the tool and patches your rc. no clone, no
package manager, no `make`. `--uninstall` reverses it.

**`simpledir update`.** checks GitHub for a newer release, shows you what it
found, and asks before installing. the download is verified (is it really
simpledir? is it really newer?) and swapped in atomically, so a failed update
can't leave you with a broken binary. there's a quiet once-a-day nudge too, off
in scripts and disableable.

**`simpledir import`.** `simpledir import ~/Projects` binds every project
directory at once. `--depth`, `--prefix`, `--hidden`, `--dry-run`, `--force`.
the "i have twelve directories and don't want to type it twelve times" command.

**`simpledir doctor`.** points at what's actually wrong: aliases whose directory
is gone (with the rebind command), names with spaces, a missing shell wrapper, a
`~/.local/bin` that isn't in your PATH. exits non-zero if something needs you.

**release assets.** every release ships `simpledir` and `install.sh` as
downloadable assets, which is what `update` and the curl installer fetch.

## what's new in 2.0

**subdirectory jumps.** `sd dots/nix` is `<dots>/nix`. costs nothing to implement
and it turns one bookmark into a whole tree, so you bind the three places you
actually visit instead of forty leaf directories.

**`simpledir rename`.** the alias you picked at 2am was the wrong name.
rename keeps the path and doesn't touch your shell config.

**`simpledir add` with fewer arguments.** `cd dotfiles && simpledir add` binds it
as `dotfiles` — the directory names itself. if you gave an explicit name that
isn't the directory name, it prints the `rename` command you probably want.

**`simpledir edit`.** the config *is* the UI, so open it in `$EDITOR`. this
creates the file with an empty config if it doesn't exist yet.

**filtering and machine-readable output.** `simpledir ls hy` lists only aliases
matching `hy` in either the name or the path. `simpledir ls --names` is one name
per line and `simpledir ls --json` emits the exact shape of the config file, so
both are easy to consume from a script or an agent.

**shell completions.** `simpledir completions bash` / `zsh`. tab-completes your
alias names, and falls through to directory completion after a `/` so `sd
dots/<tab>` keeps working.

## how the cd works

a subprocess can't change your shell's cwd. so `simpledir init` prints a shell
function that does the `cd` and gets the path from `simpledir jump`:

```bash
_simpledir_jump() {
  case "${1-}" in
    -)  builtin cd -- "$OLDPWD" && return $? ;;
    "") builtin cd -- "$HOME" && return $? ;;
  esac
  local _sd_dir
  _sd_dir=$(command simpledir jump "$@") || return $?
  builtin cd -- "$_sd_dir"
}

sd() { _simpledir_jump "$@"; }

simpledir() {
  case "${1-}" in
    "" | -)             _simpledir_jump "$@" ;;
    add|rm|rename|ls|jump|edit|completions|init|help|-h|-V|--version|--help) command simpledir "$@" ;;
    -?*)                command simpledir "$@" ;;
    *)                  _simpledir_jump "$@" ;;
  esac
}
```

the `simpledir` function shadows the binary but only intercepts bare aliases —
subcommands fall through to the real thing via `command simpledir`.

**functions, not aliases**, on purpose: bash only expands aliases in interactive
shells, so an alias would 127 inside scripts and `eval`. called from a script,
`simpledir dots` prints the path instead — which is what you want there anyway.

tested on bash 5.2. the wrapper only uses portable constructs, so zsh should be
fine, but that's untested — tell me if it breaks.

## config

`~/.simpledir/config.json`. plain json, hand-editable, gitignored nowhere —
back it up if you want:

```json
{
  "version": 1,
  "aliases": {
    "dots": "/home/you/dotfiles",
    "hypr": "/home/you/.config/hypr"
  }
}
```

`SIMPLEDIR_CONFIG_DIR` moves it somewhere else.

## things it handles

- `~`, `$VARS` and symlinks get resolved once, at `add` time
- writes are atomic (temp file + `os.replace`) — a crash can't truncate your config
- a corrupt config tells you how to fix it instead of silently starting over
- alias pointing at a deleted dir: `jump` refuses and says how to rebind, `ls` tags it `[missing]`
- duplicate alias needs `--force`, no silent clobber
- no shell alias collisions — `sd` is the only name it takes
- `simpledir add` in a directory called `dotfiles` binds it as `dotfiles`
- a subdirectory that doesn't exist reports the *joined* path and the base alias, so the fix is obvious

## development

```bash
make test                      # 143 assertions, real bash subprocesses
vhs docs/demo.tape             # re-record the gif above
make assets                    # build dist/ for a release
make release                   # tag, push, publish with assets attached
```

working on it with an AI agent? [`AGENTS.md`](AGENTS.md) has the ground rules
and the map. feeding a doc-fetching tool? [`llms.txt`](llms.txt).

## license

MIT. do what you want with it. written and maintained by one person — see
[CONTRIBUTORS.md](CONTRIBUTORS.md), PRs welcome.
