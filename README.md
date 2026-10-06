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
  <img src="https://img.shields.io/badge/language-c%2B%2B-blue?style=flat-square&logo=c%2B%2B&logoColor=white" alt="C++">
  <img src="https://img.shields.io/badge/dependencies-curl%20only-brightgreen?style=flat-square" alt="curl only">
  <img src="https://img.shields.io/badge/shell-bash%20%7C%20zsh-informational?style=flat-square" alt="bash or zsh">
  <img src="https://img.shields.io/github/stars/noxthedevwindev-greatest/simpledir?style=flat-square" alt="stars">
</p>

---

**simpledir** binds names you choose to directories, jumps to them, and remembers
the ones you never named. state is two json files in `~/.simpledir`: one you
edit, one it doesn't. no interpreter, no database, no daemon, one C++ file.

it is two commands, split by what they do to you:

| | | |
| --- | --- | --- |
| **`sd`** | moves you, reads your config | `sd dots`, `sd dots/nix`, `sd -`, `sd ls`, `sd top`, `sd i`, `sd suggest` |
| **`sdcfg`** | changes things, never moves you | `add`, `rm`, `rename`, `import`, `bind`, `forget`, `migrate`, `zoxide`, `update`, `revert`, `doctor` |

`sd` cannot rewrite your aliases and `sdcfg` cannot change your directory. one
is safe in a pipeline, the other is safe to hit by reflex.

```bash
sd dots                  # jump to a name you chose
sd dot                   # a unique prefix is enough
sd dots/nix              # jump into a subdirectory
sd                       # -> $HOME
sd -                     # -> previous dir
sd /some/path            # or just a path, why not
sd ls                    # what can I jump to?
sd top                   # where you actually go, ranked
sd proj                  # a directory you never named? frecency finds it
```

```bash
cd ~/projects/dotfiles && sdcfg add dots     # bind this dir
sdcfg rm dots
sdcfg rename dots dotfiles
sdcfg import ~/Projects                      # bind a whole tree
sdcfg bind                                   # name everything in your history
sdcfg zoxide                                 # import from zoxide
sdcfg migrate                               # migrate an old config
sdcfg revert                                 # go back to the previous version
```

## coming from zoxide

`sdcfg zoxide` reads zoxide's own database and turns its most-visited
directories into named aliases:

```bash
$ sdcfg zoxide
top 3 directories from zoxide's database:

    94.0  /home/you/Projects/neovim-config
    61.2  /home/you/.config/hypr
    40.8  /home/you/Projects/simpledir

bind them all:
  sdcfg add neovim-config /home/you/Projects/neovim-config
  sdcfg add hypr /home/you/.config/hypr
```

`--bind` does it instead of printing it. it needs the `sqlite3` command to read
zoxide's file (`sudo pacman -S sqlite` on Arch); if zoxide isn't installed at
all, it says so and where it looked.

## install

one command. it works out your platform, downloads a prebuilt binary, or
**compiles the one source file itself** when there isn't one for your
architecture — installing a toolchain with **yay** (then pacman, then mise) if
you have no compiler. either way you get `sd` plus an `sdcfg` symlink in
`~/.local/bin` and a wired-up shell rc.

```bash
curl -fsSL https://raw.githubusercontent.com/noxthedevwindev-greatest/simpledir/v8.0.1/install.sh | bash
```

prefer to look before you pipe? that's the right instinct:

```bash
curl -fsSL https://raw.githubusercontent.com/noxthedevwindev-greatest/simpledir/v8.0.1/install.sh -o install.sh
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

no package manager, no dependencies, no interpreter. one C++ file and curl. a
PKGBUILD is in `packaging/`.

the installer works out your platform, downloads a prebuilt binary if there is
one for your architecture, and **compiles from source if there isn't** (it will
install a toolchain with yay/pacman/mise first if you have no compiler). it's
linux only, which is what it says on the tin. `bash install.sh --source` forces
the compile path; `--uninstall` reverses it.

if it finds an existing install it tells you and asks what to do, rather than
silently overwriting:

```
==> found an existing simpledir install:
==>   /home/you/.local/bin/sd (sd 6.0.0 - the next zoxide)
==>   wrapper block in /home/you/.bashrc
==>   your aliases are in ~/.simpledir and are never touched by this script

  repair   replace the binaries and refresh the wrapper (default)
  uninstall remove the binaries and the wrapper, keep your aliases
  cancel   leave everything as it is and do nothing
```

## it replaces zoxide

[zoxide](https://github.com/ajeetdsouza/zoxide) is a great tool, and this is a
drop-in replacement for it: same frecency idea, same "remember a directory I
never named" behaviour. it also remembers the directories you *did* name, which
zoxide doesn't, and it's a single C++ file with no daemon.

| | zoxide | simpledir |
| --- | --- | --- |
| remembers dirs you never named | frecency-scored | frecency-scored, same half-life idea |
| remembers dirs you *named* | no | yes — that's the point |
| state to maintain | sqlite db + a daemon + shell hooks | two json files |
| moving an install | 3 files and a hook script | one file |
| per-jump cost | sqlite query + daemon round trip | **0.8 ms**, measured |
| config you can read and hand-edit | no | yes |
| your shell history | ignored | mined by `sd suggest` |
| installing it | a package manager | `curl … \| bash` |
| coming from zoxide | — | `sdcfg zoxide` imports what it knows |

if you already use zoxide, `sdcfg zoxide` reads its database and turns its top
directories into named aliases, so you don't have to start from nothing.

they don't conflict, so you can run both for a while and compare. but if this
does everything zoxide did for you, one fewer thing is a win.

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

## what's new in 8.0.1

`sd adapt` didn't work. the shell wrapper has a literal list of the read-only verbs
it forwards, and `adapt` wasn't in it, so the wrapper treated it as an alias name:

```
$ sd adapt
sd: no alias named 'adapt'
```

the binary was fine and `sdcfg adapt` was fine. only the wrapper's copy of the list
was behind, which is why it survived 446 assertions — the tests called the binary
directly.

that list is a literal inside a generated string, so it's the easy thing to forget
when adding a verb. the test now derives the verbs from the help text and asserts
the wrapper forwards every one of them, so the next verb can't have this problem.

## what's new in 8.0

**it asks which of your directories deserve names, and remembers your answer.**

`sd adapt` looks at the frecency log and lists the directories you keep visiting
that have no name. the ones you visit every day and tab-complete every time:

```
$ sd adapt
4 directories you keep visiting that have no name, most recent first:

    8.42  wezterm     /home/you/src/wezterm-config
    4.10  dotfiles    /home/you/dotfiles
    2.03  logs        /home/you/var/logs
```

then you answer, and the answer is what makes it adaptive rather than a second
`suggest`:

```bash
sdcfg adapt --accept dotfiles    # binds it, and never asks again
sdcfg adapt --reject logs       # stops asking, and doubles how long it stays quiet
```

**saying no gets quieter each time.** seven days, then fourteen, then twenty-eight,
capped at a year. a directory you keep refusing stops taking up a slot in the list,
and one you come back to after a month gets one more chance with the reason shown:

```
  sd adapt --all
    1.00  r1   /home/you/r1   [refused, quiet for 7d]
    1.00  r2   /home/you/r2   [you said no 3x, asking once more]
```

accept with `--no-bind` to record the verdict without creating an alias. `--clear`
forgets every verdict, and says plainly that names you already bound are names, not
verdicts.

**none of this is a model.** it's two integers per directory and a comparison. no
inference, no API key, nothing off the machine. the verdicts live in
`~/.simpledir/adapt.json`, which is ours and survives being corrupt without taking a
`cd` with it.

## what's new in 7.0.3

**raw.githubusercontent caches, and the whole install story is a curl from it.**

7.0.2's fix was in git, in the GitHub API, and in the v7.0.3 tag — but
`raw.githubusercontent.com/…/main/install.sh` kept serving the *old* file, so
`curl … | bash` handed you the broken prompt. that's not a GitHub bug, it's a CDN
cache keyed on the path, and there is no way to bust it from inside a URL you're
copying.

so the installer is pinned to its own tag now:

```bash
curl -fsSL https://raw.githubusercontent.com/noxthedevwindev-greatest/simpledir/v8.0.1/install.sh | bash
```

a tagged path never changes, so the cache is always correct for the version it
names. the installer knows which version it is (`SD_VERSION`) and fetches `sd.cpp`
from that same tag, so a build-from-source produces the program it claims to. a
test asserts `install.sh` and `sd.cpp` agree, because if they drift you get one
program from a script promising another.

it also refuses an asset older than itself now, not just older than what's
installed — that combination is a cached installer meeting a cached download:

```
!! the download is v6.0.0 but this installer is v7.0.3.
!! raw.githubusercontent caches by path, so it served an older release.
```

a *newer* asset is still fine and still installed: the installer installs whatever
latest is.

## what's new in 7.0.2

the menu introduced in 7.0.1 asked its question **without printing it**. the
prompt was written to a file descriptor opened read-only, so the write failed and
the question was invisible — it only appeared to work because the default happened
to be the answer:

```
  cancel    leave everything as it is and do nothing

repair                              ← your typing, with no question above it
==> repairing
```

the test missed it because under `script` stdin is a terminal, so it took the
`[ -t 0 ]` branch and never touched the `/dev/tty` path that `curl | bash` actually
uses. the test now runs the installer with stdin closed and the terminal still
available, which is what the pipe looks like from in there, and asserts the prompt
is *displayed* rather than inferring it from the answer working.

## what's new in 7.0.1

the installer had two bugs that only `curl … | bash` could reach, which is to say
the way everyone installs it.

**the "what do you want to do about the existing install?" menu could never
appear.** it checked `[ -t 0 ]`, and when the script arrives through a pipe stdin
*is* the pipe — so the documented install path silently skipped the question and
repaired without asking. it now reads `/dev/tty`, which is still your keyboard in
that situation.

**a stale download silently downgraded you.** `releases/latest/download/` is a
redirect, and redirects get cached by whatever sits between you and GitHub. handed
the previous release, the installer checked "is this our tool?", saw yes, and
installed it over a newer one. it did that to 6.5.0 while 7.0.0 was published. it
now compares versions and refuses:

```
!! the download is v6.0.0 but v7.0.0 is already installed.
!! that's a stale mirror or cache of releases/latest/download, not a real downgrade.
!! not installing it. to override: SIMPLEDIR_ALLOW_DOWNGRADE=1 bash install.sh
!! or build from the source you already have: bash install.sh --source
```

also: the menu now prints *before* the question instead of asking you to pick from
options you hadn't been shown, and `[ -r /dev/tty ]` is gone — the device node is
readable by permission even with no controlling terminal, so the read then died
with ENXIO and read as "asked, got nothing", which took the default.

## what's new in 7.0

**your prompt can show how you got there.** one line in your rc and the alias you
jumped by appears in your prompt, coloured, the way Starship shows the project and
branch:

```bash
eval "$(sdcfg prompt bash)"     # or zsh
```

```
dots ❯ cd dotfiles/hypr
```

```
❯ sd dots
dots ❯
```

the interesting part is what happens when you `cd` somewhere by hand. the segment
disappears, with no hook and no overridden `cd`, because `sd` records both the
alias *and* the directory it landed in, and the prompt only draws the marker while
`$PWD` still matches. move somewhere else and it stops matching on its own.

`suffix` jumps show what you typed (`dots/hypr`), `sd -` and raw paths clear it,
and a jump that fails leaves nothing behind.

colour it with `SD_PROMPT_COLOR`:

```bash
SD_PROMPT_COLOR="38;5;213"     # a more lurid one
```

zsh is supported (`sdcfg prompt zsh`, via `precmd_functions`) but has not been run
on this machine — there is no zsh here — so it is shape-tested only. bash is
tested for real, including that calling the hook twice doesn't stack two copies of
the segment.

## what's new in 6.5

going back past v6.0.0 now hands you a way back that needs **nothing**:

```
  heads up: v4.0.0 is the old python build. it has no `sdcfg revert` and no `update --to`,
  so it cannot bring you back here. to undo this, in a new shell:

    cp /home/you/.local/bin/sd.previous /home/you/.local/bin/sd
```

the binary it just replaced is sitting right there. telling someone to `curl | bash`
a script off the internet to undo a local change is three steps and a network where
zero would do — and the installer is only mentioned now for the case where
`sd.previous` genuinely isn't there, which it checks rather than assumes.

## what's new in 6.4

**`--to` works for every version ever published, including the two that have no
binary at all.** v1.0.0 and v2.0.0 were never given a release asset — but the
program is right there in the tag, one executable file called `simpledir`. so
`--to` falls back to the source of the tag and tells you that's what it did:

```
$ sdcfg update --to v2.0.0
  that release has no binary attached, so this is the source file from the tag
```

and if the tag's own `--version` disagrees with the tag name, it says so instead
of quietly believing one of them:

```
  note: you asked for v1.0.0, and that release's own --version says 2.0.0.
  the tag was cut from a commit whose version string had already moved on.
```

(that's a real historical mistake in this repo's tags, not a hypothetical: the
v1.0.0 tag contains a program whose VERSION string says 2.0.0. it stays that way
— rewriting a published tag helps nobody.)

## what's new in 6.3

your config file was mode `744` — executable. `write_atomic` asked for
`fs::perms::owner_all` on the non-executable path, and `owner_all` includes the
`x` bit. `config.json` and `history.json` now come out `644`, which is what a
data file should be.

## what's new in 6.2

**the update check had never worked. not once.** two bugs, stacked, both
reporting the same lie — "couldn't reach GitHub" — while reaching GitHub
perfectly well:

1. the release-list URL had no `/releases` on the end of it, so every check
   fetched the *repository* object and looked for `tag_name` inside it
2. the hand-rolled JSON scan that read the answer cut each object short at the
   literal `{?name,label}` that sits inside github's `upload_url` **string**

so `sdcfg update`, `sdcfg update --check`, `sdcfg releases` and the daily nudge
have all been silently doing nothing since 6.0.0. they now use the JSON parser
this file already had, and they work — verified against the real API.

the nudge was the worst of it, because it swallows errors on purpose: a feature
designed to be silent was hiding a total failure.

## what's new in 6.1

`update --to` and `revert` work now. in 6.0.0 they were wired to the wrong asset
name — every release publishes `sd-linux-<arch>`, and the code asked for `sd` —
so they 404'd against github 100% of the time. `--to` also no longer needs a
round trip to GitHub's API first, which is what made it fail with "couldn't
reach GitHub" while rate-limited, even though it already knew the version you
asked for.

`revert` now refuses to pretend: if `sd.previous` is the version you're already
running, it says there's nothing to do instead of writing the same file over
itself and reporting success.

## what's new in 6.0

**rewritten in C++.** one source file, no interpreter, no dependencies beyond
curl. the python version this replaces measured **73 ms per jump**; this one
measures **0.8 ms** on the same machine. that is what made the next item
possible.

**it remembers directories you never named.** zoxide's actual superpower, now
here. every directory you jump to is logged, and `sd <word>` falls back to a
frecency search when the word isn't an alias:

```bash
$ sd wev                # never bound, never typed in full
/home/you/src/wezterm-config
```

frecency is a three-day half-life, computed lazily at read time, so nothing runs
in the background. the log is throttled (one write per directory per minute),
pruned of anything that has faded, and capped at 500 entries.

**`sd top`.** the log, ranked, with what's gone and what's named marked.

**`sdcfg forget`.** `--all` wipes it, `--missing` cleans up directories that no
longer exist, or name a path. `SIMPLEDIR_NO_HISTORY=1` turns recording off.

**config version 2, and `sdcfg migrate` to get there.** two files now —
`config.json` for you, `history.json` for the tool — and a migration that backs
up before it touches anything.

**`sdcfg zoxide`.** imports what zoxide already knows about where you go.

**`sdcfg revert` and `--to`.** `revert` swaps back to the version that was
installed before the last update, offline and instantly; `--to v6.1.0` installs
a specific one. every install keeps the binary it replaced at `sd.previous`.

**`sdcfg releases`** lists what's published and what you're running.

**the installer detects and asks.** run it again over an existing install and it
tells you what it found and asks: repair, uninstall, or cancel. it also fetches
the right binary for your architecture, or compiles from source when there isn't
one, and installs a toolchain if you have no compiler.

## config

two files in `~/.simpledir`:

```
config.json     yours. plain json, hand-editable, or `sdcfg edit`
history.json    the tool's. frecency counts. don't hand-edit it, it'll cope,
                but `sdcfg forget` is the honest way to change it
```

```json
{
  "version": 2,
  "history": true,
  "aliases": {
    "dots": "/home/you/dotfiles",
    "hypr": "/home/you/.config/hypr"
  }
}
```

`"history": false` turns recording off, same as `SIMPLEDIR_NO_HISTORY=1`.

coming from version 1? `sdcfg migrate` migrates it: backs up the old file,
keeps every alias exactly as it was, and creates the (empty) history file.
`--dry-run` shows what it would do first.

`SIMPLEDIR_CONFIG_DIR` moves the whole directory somewhere else.

## commands

```bash
sd ls [<query>] [--long]      list the aliases you can jump to
sd ls --names                 one alias per line (for completion scripts)
sd ls --json                  same shape as the config file, for scripts and agents
sd print <alias[/sub]>        print the directory (what the wrapper calls)
sd top [<query>] [--json]     the frecency log, best first
sd suggest [<query>] [--top N] [--json]   mine your shell history for dirs worth naming
sd i [<query>]                interactive picker: fzf if installed

sdcfg add [<name>] [<path>]   bind a name (name defaults to the dir's own name, path to $PWD)
sdcfg rm <name>               unbind
sdcfg rename <old> <new>      rename, keep the path
sdcfg import <dir>            bind every subdirectory of a tree at once
sdcfg bind                    create aliases from what `sd suggest` found
sdcfg forget                  drop things from the visit log (--all, --missing, --path P)
sdcfg migrate                migrate the config file v1 -> v2
sdcfg zoxide                  import the directories zoxide knows about (--bind)
sdcfg edit                    open the config in $EDITOR
sdcfg update                  check for a newer release, install it if you want
sdcfg revert                  go back to the previously installed version
sdcfg releases                list the published releases
sdcfg uninstall [--purge]     remove the binaries and the shell wrapper
sdcfg doctor                  check your install: dead aliases, rc wiring, PATH
sdcfg completions bash|zsh    print a completion script
sdcfg init                    print the shell wrapper
```

every flag is long-form: `--force`, `--long`, `--dry-run`, `--keep-symlinks`,
`--top`, `--to`, `--purge`, `--yes`. no short aliases to memorise or misremember.

## staying up to date

```bash
sdcfg update              # check, then ask before installing
sdcfg update --check      # just tell me. exit 1 means an update exists (CI-friendly)
sdcfg update --yes        # don't ask
sdcfg update --to v6.1.0  # install a specific version
sdcfg revert              # go back to the version before the last update
sdcfg revert --to v6.0.0  # go back to a specific version
sdcfg releases            # what's published, and which one you're on
```

`update` downloads the release asset, runs it with `--version` to check it's
really simpledir and really newer, then swaps your binary in atomically. it
won't overwrite a working install with a same-version or garbage download, and
every install keeps what it replaced at `sd.previous`, which is what `revert`
puts back — offline, instantly, no network.

`--to` works for **every published version**, not just the newest. the asset
name has changed twice (`sd-linux-x86_64`, then `sd`, then `simpledir`), so it
tries each spelling, and it accepts a v3/v4 build that calls itself `simpledir`
rather than `sd`. it also needs no network round trip to GitHub's API, so it
works even when you're rate-limited or offline.

going back that far is a **one-way door**: versions before 6.0.0 are the python
build and have neither `revert` nor `update --to`, so they can't bring you
forward. `update --to` says so before it does it, and the way back is the
installer:

```bash
curl -fsSL https://raw.githubusercontent.com/noxthedevwindev-greatest/simpledir/v8.0.1/install.sh | bash
```

it also checks on its own, at most **once a day**, and only when you're sitting
at a terminal — never on the jump path, never in a script, never if stderr isn't
a tty. all it does is print one line:

```
sd: v6.1.0 is out (you're on v6.0.0). `sdcfg update` installs it.
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
make                # build: one g++ invocation over one file
make test           # 397 assertions, real bash subprocesses, no network
make assets         # dist/ for a release: the binary, sd.cpp, install.sh
make release        # tag, push, publish with assets attached
vhs docs/demo.tape  # re-record the gif above (needs vhs + ttyd)
```

one source file, `sd.cpp`, and one output, `sd`. the test suite drives the
compiled binary through the same command-line surface a user does, so a
behavioural regression in C++ fails the same assertions the python version had
to pass.

working on it with an AI agent? [`AGENTS.md`](AGENTS.md) has the ground rules
and the map. feeding a doc-fetching tool? [`llms.txt`](llms.txt).

## license

MIT. do what you want with it. written and maintained by one person — see
[CONTRIBUTORS.md](CONTRIBUTORS.md), PRs welcome.