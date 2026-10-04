<p align="center">
  <img src="docs/demo.gif" width="820" alt="sd demo: bind a name, jump to it, typo suggestions">
</p>

<h1 align="center">simpledir</h1>

<p align="center">
  <b>the next zoxide</b>
</p>

<p align="center">
  <code>sd</code> moves you. <code>sdcfg</code> configures things. that's the whole tool.
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-blue?style=flat-square" alt="MIT License"></a>
  <img src="https://img.shields.io/badge/python-3.8%2B-blue?style=flat-square&logo=python&logoColor=white" alt="Python 3.8+">
  <img src="https://img.shields.io/badge/dependencies-zero-brightgreen?style=flat-square" alt="zero dependencies">
  <img src="https://img.shields.io/badge/shell-bash%20%7C%20zsh-informational?style=flat-square" alt="bash or zsh">
  <img src="https://img.shields.io/github/stars/noxthedevwindev-greatest/simpledir?style=flat-square" alt="stars">
</p>

---

**simpledir** binds names you choose to directories, and jumps to them. state is a
single JSON file at `~/.simpledir/config.json`. no frecency scoring, no database,
no daemon, no dependencies beyond the Python standard library.

it is two commands, split by what they do to you:

| | | |
| --- | --- | --- |
| **`sd`** | moves you, reads your config | `sd dots`, `sd dots/nix`, `sd -`, `sd ls`, `sd i`, `sd suggest` |
| **`sdcfg`** | changes things, never moves you | `sdcfg add`, `rm`, `rename`, `import`, `bind`, `edit` |

`sd` cannot write your config and `sdcfg` cannot change your directory. one is
safe in a pipeline, the other is safe to hit by reflex.

```bash
sd dots                  # jump
sd dot                   # a unique prefix is enough
sd dots/nix              # jump into a subdirectory
sd                       # -> $HOME
sd -                     # -> previous dir
sd /some/path            # or just a path, why not
sd ls                    # what can I jump to?
```

```bash
cd ~/projects/dotfiles && sdcfg add dots     # bind this dir
sdcfg rm dots
sdcfg rename dots dotfiles
sdcfg import ~/Projects
sdcfg bind                                # name everything in your history
```

## install

one command. it checks for python, installs it with **yay** (then pacman, then
mise) if it's missing, drops the tool in `~/.local/bin` as `sd` plus an `sdcfg`
symlink, and wires your shell rc:

```bash
curl -fsSL https://raw.githubusercontent.com/noxthedevwindev-greatest/simpledir/main/install.sh | bash
```

prefer to look before you pipe? that's the right instinct:

```bash
curl -fsSL https://raw.githubusercontent.com/noxthedevwindev-greatest/simpledir/main/install.sh -o install.sh
less install.sh && bash install.sh
```

`bash install.sh --uninstall` removes both binaries and the rc block (your
aliases in `~/.simpledir/config.json` stay). installer knobs:
`SIMPLEDIR_BIN_DIR`, `SIMPLEDIR_RC`, `SIMPLEDIR_PM`, `SIMPLEDIR_NO_RC=1`.

by hand, if you'd rather:

```bash
git clone https://github.com/noxthedevwindev-greatest/simpledir
cd simpledir && make install
```

no package manager, no dependencies, python 3.8+. a PKGBUILD is in `packaging/`.

## why not zoxide

[zoxide](https://github.com/ajeetdsouza/zoxide) is a great tool and this is not
a replacement for it. the difference is what each one optimizes for:

| | zoxide | simpledir |
| --- | --- | --- |
| remembers dirs you never named | yes, frecency-scored | no |
| state to maintain | sqlite db + daemon + shell hooks | one json file |
| recall a path from 6 months ago | `z foo` and it probably knows | `sd ls` and you look |
| startup cost | shell hook + db queries | one `cat` |
| config you can read and hand-edit | no | yes |
| moving a dir | move it, forget the alias | `sdcfg add --force name /new/path` |
| installing it | a package manager | `curl … \| bash` |
| learning where you go | frecency database | reads your shell history |
| interactive picker | `zi`, needs fzf | `sd i`, fzf optional |

**use zoxide** if you wander — frecency scoring is genuinely good and
reimplementing it would double this tool's size and add a database to a project
whose whole pitch is "no database".

**use simpledir** if you `cd` to the same dozen places every day and want them
to have names. `sd suggest` borrows the useful half of that idea without the
persistence: your shell already logs where you go.

**use both.** they don't conflict — different prefixes, and they can even share
a name because each keeps its own state.

## getting started when you have nothing

don't know what to name? your shell already wrote down where you go:

```bash
$ sd suggest
2841 history commands from 1 file(s), 24 distinct directories
4 already bound, showing the top 3:

     37x  /home/you/Projects/simpledir
     19x  /home/you/.config/hypr
      8x  /home/you/src/neovim-config

bind them all:
  sdcfg add simpledir /home/you/Projects/simpledir
  sdcfg add hypr /home/you/.config/hypr
```

`sdcfg bind` does that for you instead of printing it, naming each after its
directory. this is the genuinely useful half of the frecency idea, minus the
persistence: your shell keeps a history file anyway, so read that rather than
maintaining a second record of the same thing. it understands bash, zsh and fish
history, and `--json`, `--top N` and `SIMPLEDIR_HISTORY=/path` are there too.

one honest limitation: history is mostly *relative* (`cd Projects`), and nothing
records which directory you were standing in when you typed it. those are
resolved against `$HOME`, which is right most of the time and skipped otherwise.
absolute paths are always exact.

or you have a directory of projects:

```bash
$ sdcfg import ~/Projects
bound 12: dotfiles hyprland nixdots projects simpledir ...
```

too many to scroll? pick one instead of typing:

```bash
$ sd i
$ sd i hy        # filtered
```

fzf if it's installed, otherwise a numbered list you can answer with a number or
a name. `SIMPLEDIR_NO_FZF=1` forces the list.

## typos and dead directories

because `sd dotfile` shouldn't just fail:

```
$ sd dotfile
sd: no alias named 'dotfile'
  did you mean: dotfiles
  see them all: sd ls
```

and a directory that moved is a clear error with the fix in it, not a silent
no-op. `sd ls` tags the ones that are gone `[missing]`, and `sdcfg doctor`
lists them all with rebind commands.

## symlinks

`sdcfg add` resolves symlinks by default, so `~/dotfiles/current` is stored as
whatever it points at — which keeps working when you re-point it at a new
checkout. sometimes you want the opposite, with Nix or a dotfiles repo you switch
branches in:

```bash
sdcfg add dots --keep-symlinks ~/dotfiles/current
```

that's zoxide's `_ZO_RESOLVE_SYMLINKS=0` as a flag instead of an environment
variable. reading an alias never resolves anything, so a path you asked to keep
symlinked stays symlinked.

## how the cd works

a subprocess can't change your shell's cwd. so `sdcfg init` prints a shell
function that does the `cd` and gets the path from `sd print`:

```bash
_sd_jump() {
  case "${1-}" in
    -)  builtin cd -- "$OLDPWD" && return $? ;;
    "") builtin cd -- "$HOME" && return $? ;;
    /* | ~*)  builtin cd -- "$1" && return $? ;;
  esac
  local _sd_dir
  _sd_dir=$(command sd print "$@") || return $?
  builtin cd -- "$_sd_dir"
}

sd() {
  case "${1-}" in
    ls|i|suggest|print)  command sd "$@" ;;
    -)        _sd_jump "$@" ;;
    "")       _sd_jump "$@" ;;
    -?*)      command sd "$@" ;;
    *)        _sd_jump "$@" ;;
  esac
}
```

only `sd` needs a wrapper, because only `sd` changes directory. `sdcfg` is a
plain executable and can't be shadowed by accident.

**a function, not an alias**, on purpose: bash only expands aliases in
interactive shells, so an alias would 127 inside scripts and `eval`. called from a
script, `sd dots` prints the path instead of trying to cd — which is what you
want there anyway.

tested on bash 5.2. the wrapper only uses portable constructs, so zsh should be
fine, but that's untested — tell me if it breaks.

## config

`~/.simpledir/config.json`. plain json, hand-editable, or `sdcfg edit` which is
the same file in `$EDITOR`:

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

## commands

```bash
sd ls [<query>] [--long]      list the aliases you can jump to
sd ls --names                 one alias per line (for completion scripts)
sd ls --json                  same shape as the config file, for scripts and agents
sd print <alias[/sub]>        print the directory (what the wrapper calls)
sd suggest [--top N] [--json]  mine your shell history for dirs worth naming
sd i [<query>]                interactive picker: fzf if installed

sdcfg add [<name>] [<path>]   bind a name (name defaults to the dir's own name, path to $PWD)
sdcfg rm <name>               unbind
sdcfg rename <old> <new>      rename, keep the path
sdcfg import <dir>            bind every subdirectory of a tree at once
sdcfg bind                    create aliases from what `sd suggest` found
sdcfg edit                    open the config in $EDITOR
sdcfg update                  check for a newer release, install it if you want
sdcfg uninstall [--purge]     remove the binaries and the shell wrapper
sdcfg doctor                  check your install: dead aliases, rc wiring, PATH
sdcfg completions bash|zsh    print a completion script
sdcfg init                    print the shell wrapper
```

every flag is long-form: `--force`, `--long`, `--dry-run`, `--keep-symlinks`,
`--purge`, `--yes`. no short aliases to memorise or misremember.

## staying up to date

```bash
sdcfg update              # check, then ask before installing
sdcfg update --check      # just tell me. exit 1 means an update exists (CI-friendly)
sdcfg update --yes        # don't ask
```

`update` downloads the release asset, checks that it's actually simpledir and
actually newer, then replaces your binary atomically. it won't overwrite a
working install with a same-version or garbage download.

it also checks on its own, at most **once a day**, and only when you're sitting
at a terminal — never on the jump path, never in a script, never if stderr isn't
a tty. all it does is print one line:

```
sd: v5.1.0 is out (you're on v5.0.0). `sdcfg update` installs it.
```

opt out entirely with `SIMPLEDIR_NO_UPDATE_CHECK=1`.

## uninstalling

```bash
sdcfg uninstall              # remove the binaries and the wrapper, ask first
sdcfg uninstall --purge      # also delete ~/.simpledir and every alias
sdcfg uninstall --yes        # no prompt, for scripts
```

it deletes the running script (fine on unix — the inode outlives the process),
rewrites your shell rc with the marked block removed, and keeps a timestamped
backup of the rc file. your aliases are **data, not installation**, so they stay
unless you ask for `--purge`; it tells you that option exists either way.

it removes **the install**, not necessarily the copy you're running: it targets
`~/.local/bin/sd` if that's there, otherwise the file you're executing. so
running a checkout copy still cleans up the installed one. point it somewhere
else with `SIMPLEDIR_BIN=/path/to/sd`.

## development

```bash
make test                      # 235 assertions, real bash subprocesses
vhs docs/demo.tape             # re-record the gif above
make assets                    # build dist/ for a release
make release                   # tag, push, publish with assets attached
```

working on it with an AI agent? [`AGENTS.md`](AGENTS.md) has the ground rules
and the map. feeding a doc-fetching tool? [`llms.txt`](llms.txt).

## license

MIT. do what you want with it. written and maintained by one person — see
[CONTRIBUTORS.md](CONTRIBUTORS.md), PRs welcome.