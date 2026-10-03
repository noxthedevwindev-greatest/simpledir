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

**use zoxide** if you wander. if you `cd` to the same dozen places every day and
want them to have names, this is less machinery for the same result.

**use both.** they don't conflict — different prefixes.

## install

```bash
git clone https://github.com/noxthedevwindev-greatest/simpledir
cd simpledir && make install
```

`make install` drops the binary in `~/.local/bin` and appends a guarded block to
your `~/.bashrc`. prefer to do it by hand:

```bash
install -Dm755 simpledir ~/.local/bin/simpledir
simpledir init >> ~/.bashrc      # or ~/.zshrc
```

no package manager, no dependencies, python 3.8+. not in the AUR yet — PR welcome.

## usage

```bash
simpledir add <name> [path]   # bind a name. path defaults to $PWD. -f overwrites
simpledir rm <name>           # unbind
simpledir ls [-l]             # list. -l for absolute paths
simpledir jump <name>         # print the path, don't cd
simpledir init                # print the shell wrapper
```

and in your shell, `sd` / `simpledir` with no subcommand:

```bash
sd dots        # cd to the bound dir
sd             # -> $HOME
sd -           # -> previous dir
simpledir dots # identical to `sd dots`
```

typo suggestions, because `sd dotfile` shouldn't just fail:

```
$ sd dotfile
simpledir: no alias named 'dotfile'
  did you mean: dotfiles
  see them all: simpledir ls
```

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
    add|rm|ls|jump|init|help|-h|-V|--version|--help) command simpledir "$@" ;;
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

## development

```bash
make test                      # 37 assertions, real bash subprocesses
vhs docs/demo.tape             # re-record the gif above
```

working on it with an AI agent? [`AGENTS.md`](AGENTS.md) has the ground rules
and the map. feeding a doc-fetching tool? [`llms.txt`](llms.txt).

## license

MIT. do what you want with it. written and maintained by one person — see
[CONTRIBUTORS.md](CONTRIBUTORS.md), PRs welcome.
