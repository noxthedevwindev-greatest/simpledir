#!/usr/bin/env bash
# simpledir test suite. isolated config + a real bash that sources the wrapper.
#
# two commands, one program: `sd` reads and moves, `sdcfg` writes.
# $SD is the move half, $CFG the config half, both copies of the same file.
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
export SIMPLEDIR_CONFIG_DIR=$(mktemp -d)
trap 'rm -rf "$SIMPLEDIR_CONFIG_DIR"' EXIT

# two names, one file. the mode is decided by argv[0], exactly like the install.
SD="$ROOT/sd"
CFG="$ROOT/sdcfg"
ln -sf sd "$CFG"

pass=0 fail=0

check() { # check <label> <expected-exit> <command...>
  local label=$1 want=$2; shift 2
  local out; out=$("$@" 2>&1); local got=$?
  if [[ $got -eq $want ]]; then
    pass=$((pass + 1)); printf 'ok   %s\n' "$label"
  else
    fail=$((fail + 1)); printf 'FAIL %s (exit %s, wanted %s)\n%s\n' "$label" "$got" "$want" "$out"
  fi
}

contains() { # contains <label> <needle> <command...>
  local label=$1 needle=$2; shift 2
  local out; out=$("$@" 2>&1)
  if [[ $out == *"$needle"* ]]; then
    pass=$((pass + 1)); printf 'ok   %s\n' "$label"
  else
    fail=$((fail + 1)); printf 'FAIL %s (missing %q)\n%s\n' "$label" "$needle" "$out"
  fi
}

lacks() { # lacks <label> <needle> <command...>
  local label=$1 needle=$2; shift 2
  local out; out=$("$@" 2>&1)
  if [[ $out != *"$needle"* ]]; then
    pass=$((pass + 1)); printf 'ok   %s\n' "$label"
  else
    fail=$((fail + 1)); printf 'FAIL %s (should not contain %q)\n%s\n' "$label" "$needle" "$out"
  fi
}

has() { # has <label> <needle> <text> - for output you already captured
  local label=$1 needle=$2 text=$3
  if [[ $text == *"$needle"* ]]; then
    pass=$((pass + 1)); printf 'ok   %s\n' "$label"
  else
    fail=$((fail + 1)); printf 'FAIL %s (missing %q)\n%s\n' "$label" "$needle" "$text"
  fi
}

# the update nudge only fires on a terminal, so those checks get a pty
on_tty() { script -qec "$*" /dev/null; }

# --- the split itself --------------------------------------------------------
check "both names run"            0 "$SD" --version
check "both names run"            0 "$CFG" --version
contains "sd calls itself sd"    "sd " "$SD" --version
contains "sdcfg calls itself sdcfg" "sdcfg " "$CFG" --version

# sd must not answer config verbs, and should point at sdcfg when it doesn't
check "sd refuses to add"         2 "$SD" add somewhere /tmp
contains "sd points at sdcfg"    'sdcfg add' "$SD" add somewhere /tmp
check "sd refuses rm"            2 "$SD" rm whatever
check "sd refuses import"         2 "$SD" import /tmp
contains "sd says it only reads" "only reads your config" "$SD" add somewhere /tmp

# sdcfg must not answer move verbs, and should point at sd when it doesn't
check "sdcfg refuses a bare alias" 2 "$CFG" whatever
check "sdcfg refuses a move verb"  2 "$CFG" ls
contains "sdcfg points at sd"    'sd ls' "$CFG" ls
contains "sdcfg says it only writes" "only changes things" "$CFG" ls
check "sdcfg refuses ls"          2 "$CFG" ls
check "sdcfg refuses i"           2 "$CFG" i

# --- sdcfg add ---------------------------------------------------------------
check "add binds cwd"          0 "$CFG" add here
check "add with explicit path" 0 "$CFG" add home "$HOME"
check "add tilde path"         0 "$CFG" add dotdir "$HOME/.config"
check "add rejects bad path"   1 "$CFG" add nope /does/not/exist
check "add no clobber"         1 "$CFG" add here
check "add --force"            0 "$CFG" add --force here "$HOME"

# --- sd ls -------------------------------------------------------------------
contains "ls lists alias"      "here"  "$SD" ls
contains "ls --long absolute"  "$HOME" "$SD" ls --long
while read -r name; do "$CFG" rm "$name" >/dev/null; done < <("$SD" ls --names)
check "ls empty exits 1"       1 "$SD" ls

# --- sd print / jump ---------------------------------------------------------
"$CFG" add home "$HOME" >/dev/null
"$CFG" add dotdir "$HOME/.config" >/dev/null
check "print resolves"         0 "$SD" print home
contains "print echoes path"   "$HOME" "$SD" print home
check "bare name jumps"        0 "$SD" home
contains "bare form echoes path" "$HOME" "$SD" home
check "unknown exits 1"        1 "$SD" print zzzz
contains "suggests near miss"  "did you mean" "$SD" print hoem
contains "suggests a typo"     "did you mean" "$SD" print dotdirx
check "no args = help"         0 "$SD"

# --- prefix matching ---------------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
"$CFG" add hypr "$HOME" >/dev/null
"$CFG" add hyprland "$HOME" >/dev/null
"$CFG" add kbd "$HOME" >/dev/null
check "unique prefix jumps"        0 "$SD" print kbd
contains "unique prefix resolves"  "$HOME" "$SD" print kbd
"$CFG" rename hyprland hl >/dev/null
check "prefix after a rename"      0 "$SD" print hy
check "exact name beats prefix"    0 "$SD" print hypr
check "ambiguous prefix fails"     1 "$SD" print h
contains "ambiguous prefix lists them" "hl hypr" "$SD" print h

# --- subdirectory suffixes ---------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/tree/src/deep"
"$CFG" add tree "$SIMPLEDIR_CONFIG_DIR/tree" >/dev/null
contains "suffix resolves"     "/tree/src"          "$SD" print tree/src
contains "deep suffix"         "/tree/src/deep"     "$SD" print tree/src/deep
contains "suffix normalizes .." "/tree/src"         "$SD" print tree/src/../src
contains "trailing slash ok"   "/tree"              "$SD" print tree/
check "missing suffix fails"   1 "$SD" print tree/nope
contains "missing suffix names it" "tree/nope"       "$SD" print tree/nope
check "absolute path not an alias" 1 "$SD" print /tmp
contains "absolute path says so" "isn't an alias"    "$SD" print /tmp

# --- sdcfg rm / rename -------------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
"$CFG" add dots "$HOME" >/dev/null
check "rename works"           0 "$CFG" rename dots dotfiles
contains "rename keeps path"   "$HOME" "$SD" print dotfiles
check "old name is gone"       1 "$SD" print dots
"$CFG" add other /tmp >/dev/null
check "rename no clobber"      1 "$CFG" rename dotfiles other
check "rename --force"         0 "$CFG" rename --force dotfiles other
check "rename unknown fails"   1 "$CFG" rename nope whatever
"$CFG" add gone /tmp >/dev/null
check "rm removes"             0 "$CFG" rm gone
contains "rm gone says gone"   "gone" "$CFG" rm gone
check "rm unknown fails"       1 "$CFG" rm nope

# --- missing target ----------------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/tmpdir"
"$CFG" add vanish "$SIMPLEDIR_CONFIG_DIR/tmpdir" >/dev/null
rmdir "$SIMPLEDIR_CONFIG_DIR/tmpdir"
check "print refuses missing"  1 "$SD" print vanish
contains "ls tags missing"     "[missing]" "$SD" ls

# --- config integrity --------------------------------------------------------
contains "config is json" '"aliases"' cat "$SIMPLEDIR_CONFIG_DIR/config.json"
check "no temp files left"     0 bash -c "! compgen -G '$SIMPLEDIR_CONFIG_DIR/.config.*'"
printf 'not json{' > "$SIMPLEDIR_CONFIG_DIR/config.json"
contains "corrupt config warns" "not valid JSON" "$SD" ls

# --- sdcfg add with fewer arguments ------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/dotfiles"
contains "add derives name"    "dotfiles ->" bash -c "cd '$SIMPLEDIR_CONFIG_DIR/dotfiles' && '$CFG' add --force"
check "add on / is nameable"   0 "$CFG" add rootdir /
contains "add name only"       "mydir ->" bash -c "cd '$SIMPLEDIR_CONFIG_DIR' && '$CFG' add mydir"

# --- sd ls flags and filter --------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
"$CFG" add alpha "$HOME" >/dev/null
"$CFG" add beta /tmp >/dev/null
contains "ls filters by name"  "alpha"  "$SD" ls alp
contains "ls filters by path"  "beta"   "$SD" ls tmp
check "ls filter miss exits 1" 1 "$SD" ls zzz
contains "ls filter says so"   "nothing matches" "$SD" ls zzz
contains "ls --names"          "alpha"  "$SD" ls --names
check "ls --names is one per line" 0 bash -c "'$SD' ls --names | grep -qx alpha"
contains "ls --json has aliases" '"aliases"' "$SD" ls --json
contains "ls --json has version" '"version"' "$SD" ls --json
check "ls --json is valid json" 0 bash -c "'$SD' ls --json | python3 -c 'import json,sys; json.load(sys.stdin)'"

# --- add --keep-symlinks -----------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/real"
ln -sfn "$SIMPLEDIR_CONFIG_DIR/real" "$SIMPLEDIR_CONFIG_DIR/link"
"$CFG" add resolved "$SIMPLEDIR_CONFIG_DIR/link" >/dev/null
"$CFG" add literal --keep-symlinks "$SIMPLEDIR_CONFIG_DIR/link" >/dev/null
contains "default resolves the symlink" "/real" "$SD" print resolved
contains "--keep-symlinks keeps it"      "/link" "$SD" print literal
check "both still point at a real dir"   0 "$SD" print literal
lacks "no hint when resolved"            "kept the symlink" "$CFG" add resolved2 "$SIMPLEDIR_CONFIG_DIR/link"

# --- sdcfg import ------------------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/tree"/{alpha,beta,gamma/nested/leaf} "$SIMPLEDIR_CONFIG_DIR/tree/.hidden"
contains "import binds children"  "alpha beta gamma" "$CFG" import "$SIMPLEDIR_CONFIG_DIR/tree"
check "import is idempotent"     0 "$CFG" import "$SIMPLEDIR_CONFIG_DIR/tree"
contains "import skips bound"    "skipped 3"  "$CFG" import "$SIMPLEDIR_CONFIG_DIR/tree"
contains "import with prefix"    "p-alpha"    "$CFG" import --prefix p- "$SIMPLEDIR_CONFIG_DIR/tree"
contains "import depth 2"        "g-nested"   "$CFG" import --depth 2 --prefix g- "$SIMPLEDIR_CONFIG_DIR/tree"
lacks   "import skips dotfiles" ".hidden"    "$CFG" import --depth 1 --prefix z- "$SIMPLEDIR_CONFIG_DIR/tree"
contains "import --hidden takes them" ".hidden" "$CFG" import --hidden --depth 1 --prefix h- "$SIMPLEDIR_CONFIG_DIR/tree"
contains "import --dry-run says would" "would bind" "$CFG" import --dry-run --prefix dry- --depth 1 "$SIMPLEDIR_CONFIG_DIR/tree"
check "import --dry-run writes nothing" 1 "$SD" ls dry-alpha
contains "import rejects a file"  "not a directory" "$CFG" import /etc/hostname

# --- sd suggest + sdcfg bind -------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
HIST=$(mktemp -d)
# a fake HOME, or the developer's real ~/.bash_history leaks into the counts
mkdir -p "$HIST/home" "$HIST/alpha" "$HIST/beta" "$HIST/gamma"
cat > "$HIST/home/.bash_history" <<EOF
cd $HIST/alpha
cd $HIST/alpha
cd $HIST/alpha/sub
cd $HIST/beta
cd '$HIST/gamma'
pushd $HIST/gamma
cd ~/definitely/not/here
cd
cd -
cd /tmp
ls -l $HIST/beta
EOF
sexport() { env HOME="$HIST/home" SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" "$@"; }

contains "suggest counts visits"   "2x"  sexport "$SD" suggest
contains "suggest lists a dir"     "$HIST/alpha" sexport "$SD" suggest
lacks   "suggest skips non-dirs"  "not/here"     sexport "$SD" suggest
lacks   "suggest ignores ls -l"    "ls -l"        sexport "$SD" suggest
contains "suggest emits an sdcfg add line" "sdcfg add alpha $HIST/alpha" sexport "$SD" suggest
check "suggest --json is valid json" 0 bash -c "env HOME='$HIST/home' SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' suggest --json | python3 -c 'import json,sys; json.load(sys.stdin)'"
contains "suggest --top limits"    "top 1" sexport "$SD" suggest --top 1
lacks   "suggest --top respects it" "$HIST/beta" sexport "$SD" suggest --top 1
# sd must never write: bind lives on the other half
check "sd has no bind verb"       1 "$SD" bind

contains "sdcfg bind creates" "bound 4" sexport "$CFG" bind
check "sdcfg bind wrote aliases" 0 bash -c "'$SD' ls --names | grep -qx alpha"
contains "suggest knows they're bound" "already bound" sexport "$SD" suggest
check "suggest exits 0 when all bound" 0 bash -c \
  "env HOME='$HIST/home' SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' suggest"
contains "bind --dry-run says would" "would bind" bash -c \
  "rm -rf '$SIMPLEDIR_CONFIG_DIR'; env HOME='$HIST/home' SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$CFG' bind --dry-run"

: > "$HIST/home/.bash_history"
contains "empty history explains itself" 'no `cd` targets' sexport "$SD" suggest
check "empty history exits 1"       1 sexport "$SD" suggest
mv "$HIST/home/.bash_history" "$HIST/home/.bash_history.off"
check "missing history explains itself" 1 sexport "$SD" suggest
contains "missing history lists where" ".bash_history" sexport "$SD" suggest
rm -rf "$HIST"

# relative paths are the common case, and `cd` must not match mid-argument
REL=$(mktemp -d)
mkdir -p "$REL/home/Projects/one" "$REL/home/Projects/two"
cat > "$REL/home/.bash_history" <<EOF
cd Projects
cd Projects/one
cd Projects/two
cd ./Projects/one
echo cd Projects
ls Projects
sudo cd Projects
cd Projects/one && cd Projects/two
cd nosuchdir
EOF
rel() { env HOME="$REL/home" SIMPLEDIR_CONFIG_DIR="$REL/cfg" "$@"; }
out=$(rel "$SD" suggest)
has "relative cd resolves against \$HOME" "$REL/home/Projects/one" "$out"
has "relative cd counts every visit"    "2x" "$out"
has "sudo cd counts"                    "$REL/home/Projects" "$out"
lacks "echo cd is not a cd"             "echo cd" "$out"
lacks "a nonexistent relative path is skipped" "nosuchdir" "$out"
rm -rf "$REL"

# --- sd i (picker) -----------------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
"$CFG" add alpha "$HOME" >/dev/null
"$CFG" add beta /tmp >/dev/null
STUB=$(mktemp -d)
printf '#!/usr/bin/env python3\nimport sys\nlines=sys.stdin.read().splitlines()\nprint(lines[0] if lines else "")\n' > "$STUB/fzf"
chmod +x "$STUB/fzf"

# this machine has a real fzf, so force each branch explicitly
contains "picker uses fzf when present"  "alpha" env PATH="$STUB:/usr/bin:/bin" SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" "$SD" i
contains "picker filters"                "alpha" env PATH="$STUB:/usr/bin:/bin" SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" "$SD" i alp
printf '#!/usr/bin/env bash\nexit 130\n' > "$STUB/fzf"
check "escape from fzf exits 1"         1 env PATH="$STUB:/usr/bin:/bin" SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" "$SD" i

contains "picker falls back to a list"  "pick a number" bash -c \
  "printf '2\n' | env SIMPLEDIR_NO_FZF=1 SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' i"
contains "picker answers with the name" "beta" bash -c \
  "printf '2\n' | env SIMPLEDIR_NO_FZF=1 SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' i"
contains "picker takes a typed name"    "gamma" bash -c \
  "printf 'gamma\n' | env SIMPLEDIR_NO_FZF=1 SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' i"
check "picker rejects out-of-range"     1 bash -c \
  "printf '99\n' | env SIMPLEDIR_NO_FZF=1 SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' i"
contains "picker explains out-of-range" "no entry 99" bash -c \
  "printf '99\n' | env SIMPLEDIR_NO_FZF=1 SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' i"
check "picker gives up on empty input"  1 bash -c \
  "printf '\n' | env SIMPLEDIR_NO_FZF=1 SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' i"
contains "picker says why it's a list" "SIMPLEDIR_NO_FZF" bash -c \
  "printf '\n' | env SIMPLEDIR_NO_FZF=1 SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' i"
mkdir -p "$STUB/bare"
ln -sf "$(command -v python3)" "$STUB/bare/python3"
contains "picker reports fzf missing" "no fzf installed" bash -c \
  "printf '\n' | env PATH='$STUB/bare' SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR' '$SD' i"
check "picker with nothing to pick"    1 env SIMPLEDIR_CONFIG_DIR="$STUB/empty" "$SD" i
rm -rf "$STUB"

# --- sdcfg edit --------------------------------------------------------------
EDITOR=true check "edit with a no-op editor" 0 "$CFG" edit
EDITOR=/nonexistent-binary-xyz check "edit reports bad editor" 1 "$CFG" edit
contains "edit created a config" '"aliases"' cat "$SIMPLEDIR_CONFIG_DIR/config.json"

# --- sdcfg update (offline: file:// stubs) -----------------------------------
STUBS=$(mktemp -d)
me=$("$CFG" --version | awk '{print $2}')
printf '{"tag_name": "v99.0.0"}' > "$STUBS/api_new.json"
printf '{"tag_name": "v%s"}' "$me"   > "$STUBS/api_same.json"
printf '{"tag_name": "v0.0.1"}'       > "$STUBS/api_old.json"

export SIMPLEDIR_UPDATE_URL="file://$STUBS/api_new.json"
contains "update sees a newer release" "v99.0.0" "$CFG" update --check
check "update --check exits 1 when newer" 1 "$CFG" update --check

export SIMPLEDIR_UPDATE_URL="file://$STUBS/api_same.json"
contains "update says up to date"  "you're up to date" "$CFG" update --check
check "update --check exits 0 when current" 0 "$CFG" update --check

export SIMPLEDIR_UPDATE_URL="file://$STUBS/api_old.json"
contains "old release isn't an update" "up to date" "$CFG" update --check

export SIMPLEDIR_UPDATE_URL="file://$STUBS/missing.json"
contains "unreachable api explains itself" "couldn't reach GitHub" "$CFG" update --check
check "unreachable api exits 1" 1 "$CFG" update --check

# self-install: stub asset is this script with a bumped VERSION
sed 's/^VERSION = "[^"]*"/VERSION = "99.0.0"/' "$SD" > "$STUBS/candidate"
chmod +x "$STUBS/candidate"
export SIMPLEDIR_UPDATE_URL="file://$STUBS/api_new.json"
export SIMPLEDIR_UPDATE_ASSET_URL="file://$STUBS/candidate"
export SIMPLEDIR_BIN="$STUBS/installed"
contains "update installs with --yes" "updated to v99.0.0" "$CFG" update --yes
check "installed binary works" 0 "$STUBS/installed" --version
contains "installed binary is the new version" "99.0.0" "$STUBS/installed" --version
contains "installed binary still knows its name" "sd 99.0.0" "$STUBS/installed" --version
check "no staging files left behind" 0 bash -c "! compgen -G '$STUBS/installed.*'"

printf 'not a program\n' > "$STUBS/garbage"
export SIMPLEDIR_UPDATE_ASSET_URL="file://$STUBS/garbage"
export SIMPLEDIR_BIN="$STUBS/untouched"
contains "update rejects junk" "isn't runnable" "$CFG" update --yes
check "junk never got written" 0 bash -c "! test -e '$STUBS/untouched'"

export SIMPLEDIR_UPDATE_ASSET_URL="file://$SD"
export SIMPLEDIR_BIN="$STUBS/samever"
contains "update rejects same-version asset" "same as what you have" "$CFG" update --yes
check "same-version asset wrote nothing" 0 bash -c "! test -e '$STUBS/samever'"

unset SIMPLEDIR_BIN SIMPLEDIR_UPDATE_ASSET_URL SIMPLEDIR_UPDATE_URL
rm -rf "$STUBS"

# the nudge: silent off-terminal, silent on the jump path, one line on a tty
rm -rf "$SIMPLEDIR_CONFIG_DIR"
"$CFG" add home "$HOME" >/dev/null
mkdir -p "$SIMPLEDIR_CONFIG_DIR"
printf '{"tag_name": "v99.0.0"}' > "$SIMPLEDIR_CONFIG_DIR/api_new.json"
printf '{"checked": 9999999999, "latest": "v99.0.0"}' > "$SIMPLEDIR_CONFIG_DIR/update-check.json"
check "print path stays silent"  0 env SIMPLEDIR_UPDATE_URL="file://$SIMPLEDIR_CONFIG_DIR/api_new.json" "$SD" print home
lacks   "ls stays silent off a terminal" "is out" "$SD" ls
check "jump never nudges even on a tty" 0 on_tty "SIMPLEDIR_UPDATE_URL=file://$SIMPLEDIR_CONFIG_DIR/api_new.json '$SD' print home"
contains "ls nudges on a tty" "v99.0.0 is out" on_tty "SIMPLEDIR_UPDATE_URL=file://$SIMPLEDIR_CONFIG_DIR/api_new.json '$SD' ls"
lacks   "opt-out silences the nudge" "is out" on_tty "SIMPLEDIR_NO_UPDATE_CHECK=1 '$SD' ls"

# --- sdcfg uninstall ---------------------------------------------------------
UNROOT=$(mktemp -d)
fresh() { # fresh <name> -> prints the scenario dir
  local dir="$UNROOT/$1"
  mkdir -p "$dir/bin" "$dir/cfg"
  install -m 755 "$SD" "$dir/bin/sd"
  ln -sf sd "$dir/bin/sdcfg"
  env SIMPLEDIR_CONFIG_DIR="$dir/cfg" "$dir/bin/sdcfg" add mine "$HOME" >/dev/null
  printf '# my rc\n\n# >>> simpledir >>>\nwrapper junk\n# <<< simpledir <<<\n\ntail\n' > "$dir/rc"
  printf '%s' "$dir"
}
un() { # un <dir> [args...]
  local dir=$1; shift
  env SIMPLEDIR_RC="$dir/rc" SIMPLEDIR_CONFIG_DIR="$dir/cfg" \
      SIMPLEDIR_BIN="$dir/bin/sd" SIMPLEDIR_NO_UPDATE_CHECK=1 \
      "$dir/bin/sdcfg" "$@"
}

d=$(fresh keep)
out=$(un "$d" uninstall --yes)
has "uninstall says aliases survive" "these stay"   "$out"
has "uninstall names the purge flag"  "--purge"     "$out"
has "uninstall reports the binary"    "removed"     "$out"
has "uninstall reports the rc"        "cleaned"     "$out"
has "uninstall reminds about reload"  "exec bash"   "$out"
check "uninstall removed the binary"  0 bash -c "! test -e '$d/bin/sd'"
check "uninstall cleaned the block"   0 bash -c "! grep -q '>>> simpledir >>>' '$d/rc'"
contains "rc keeps what came before"   "my rc" cat "$d/rc"
contains "rc keeps what came after"    "tail"  cat "$d/rc"
lacks   "rc block is gone"             "wrapper junk" cat "$d/rc"
check "uninstall kept a backup"        0 bash -c "compgen -G '$d/rc.bak.*' >/dev/null"
check "uninstall kept the config"      0 bash -c "test -f '$d/cfg/config.json'"
check "no staging files left behind"   0 bash -c "! compgen -G '$d/rc.new.*'"

d=$(fresh purge)
contains "purge says it deletes the config" "deleted" un "$d" uninstall --yes --purge
check "purge removed the config dir"   0 bash -c "! test -e '$d/cfg'"
check "purge removed the binary"       0 bash -c "! test -e '$d/bin/sd'"
check "purge cleaned the rc"           0 bash -c "! grep -q '>>> simpledir >>>' '$d/rc'"

d=$(fresh refuse)
check "uninstall refuses without a tty" 1 un "$d" uninstall
check "refusal changed nothing"         0 bash -c "test -x '$d/bin/sd'"
check "refusal left the rc"             0 bash -c "grep -q '>>> simpledir >>>' '$d/rc'"

d=$(fresh declined)
# the answer has to come through the pty, or stdin isn't a terminal
out=$(printf 'n\n' | on_tty "env SIMPLEDIR_RC=$d/rc SIMPLEDIR_CONFIG_DIR=$d/cfg SIMPLEDIR_BIN=$d/bin/sd SIMPLEDIR_NO_UPDATE_CHECK=1 $d/bin/sdcfg uninstall")
has "answering no changes nothing" "nothing changed" "$out"
check "binary survived the no"         0 bash -c "test -x '$d/bin/sd'"
check "rc survived the no"             0 bash -c "grep -q '>>> simpledir >>>' '$d/rc'"

d=$(fresh norc)
rm -f "$d/rc"
contains "uninstall tolerates no rc"   "no wrapper block" un "$d" uninstall --yes
check "binary still removed without an rc" 0 bash -c "! test -e '$d/bin/sd'"

d=$(fresh elsewhere)
contains "uninstall flags a different target" "not the copy you're running" env \
  SIMPLEDIR_RC="$d/rc" SIMPLEDIR_CONFIG_DIR="$d/cfg" \
  SIMPLEDIR_BIN="$UNROOT/elsewhere/sd" SIMPLEDIR_NO_UPDATE_CHECK=1 \
  "$CFG" uninstall --yes
check "the running copy was left alone" 0 bash -c "test -x '$SD'"

# SIMPLEDIR_RC is exclusive: it must not reach for the real ~/.bashrc
check "SIMPLEDIR_RC is exclusive"      0 bash -c "grep -q '>>> simpledir >>>' '$HOME/.bashrc'"
rm -rf "$UNROOT"

# --- sdcfg doctor ------------------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
check "doctor fails with no config"  1 "$CFG" doctor
contains "doctor says so"         "no config yet" "$CFG" doctor
"$CFG" add alive /tmp >/dev/null
mkdir -p "$SIMPLEDIR_CONFIG_DIR/dead"
"$CFG" add deadalias "$SIMPLEDIR_CONFIG_DIR/dead" >/dev/null
rmdir "$SIMPLEDIR_CONFIG_DIR/dead"
contains "doctor finds dead aliases" "is gone" "$CFG" doctor
contains "doctor suggests a fix"     "sdcfg add --force deadalias" "$CFG" doctor
"$CFG" add 'weird name' /tmp >/dev/null
contains "doctor flags odd names"    "whitespace" "$CFG" doctor

# --- completions -------------------------------------------------------------
contains "bash completions"     "complete" "$CFG" completions bash
contains "bash comp has sd"     " sd"      "$CFG" completions bash
contains "bash comp has sdcfg"  " sdcfg"   "$CFG" completions bash
contains "bash comp verbs"      "uninstall" "$CFG" completions bash
contains "zsh completions"      "#compdef" "$CFG" completions zsh
check "completions reject junk" 2 "$CFG" completions fish
check "generated bash comp is valid bash" 0 bash -n <("$CFG" completions bash)

# --- install.sh --------------------------------------------------------------
# offline: SIMPLEDIR_RELEASE_URL points at the repo copy through a file:// url
check "install.sh parses"        0 bash -n "$ROOT/install.sh"
contains "install.sh --help"     "curl -fsSL" bash "$ROOT/install.sh" --help
check "install.sh rejects junk"  1 bash "$ROOT/install.sh" --nope
contains "install.sh validates pm" "must be yay" env SIMPLEDIR_PM=brew bash "$ROOT/install.sh"
check "install.sh bad pm exits 1" 1 env SIMPLEDIR_PM=brew bash "$ROOT/install.sh"

FAKEHOME=$(mktemp -d)
helper="$FAKEHOME/install-here"
cat > "$helper" <<EOF
#!/usr/bin/env bash
# install into a throwaway HOME, from the local file, no network.
# \$1 is the fake home, the rest are install.sh's own arguments.
home=\$1; shift
mkdir -p "\$home"
env HOME="\$home" SHELL=/bin/bash PATH="/usr/bin:/bin" \\
    SIMPLEDIR_RELEASE_URL="file://$ROOT/sd" \\
    SIMPLEDIR_RAW_URL="file://$ROOT/sd" \\
    bash "$ROOT/install.sh" "\$@"
EOF
chmod +x "$helper"

check "install.sh installs sd"       0 env SIMPLEDIR_NO_RC=1 "$helper" "$FAKEHOME/plain"
check "sd is executable"              0 "$FAKEHOME/plain/.local/bin/sd" --version
check "sdcfg exists and runs"         0 "$FAKEHOME/plain/.local/bin/sdcfg" --version
check "sdcfg is a symlink to sd"      0 bash -c "[ \"\$(readlink '$FAKEHOME/plain/.local/bin/sdcfg')\" = sd ]"
contains "installer reports the version" "simpledir" env SIMPLEDIR_NO_RC=1 "$helper" "$FAKEHOME/again"
check "installed copy matches the repo" 0 bash -c "diff -q '$ROOT/sd' '$FAKEHOME/plain/.local/bin/sd'"
check "no staging files left behind"    0 bash -c "! compgen -G '$FAKEHOME/plain/.local/bin/.sd.*'"

check "opt-out leaves the rc alone"     0 env SIMPLEDIR_NO_RC=1 "$helper" "$FAKEHOME/norc"
check "no rc file was created"          0 bash -c "! test -e '$FAKEHOME/norc/.bashrc'"

check "install.sh wires the rc"         0 "$helper" "$FAKEHOME/withrc"
contains "rc has the begin marker" "# >>> simpledir >>>" cat "$FAKEHOME/withrc/.bashrc"
check "rc marker appears once" 0 bash -c \
  "[ \$(grep -c '>>> simpledir >>>' '$FAKEHOME/withrc/.bashrc') -eq 1 ]"
check "rc block defines sd" 0 bash -c "grep -q '^sd()' '$FAKEHOME/withrc/.bashrc'"
check "rc block mentions sdcfg" 0 bash -c "grep -q 'changes things and never moves you' '$FAKEHOME/withrc/.bashrc'"
"$CFG" add home "$HOME" >/dev/null
check "wired shell actually jumps" 0 env -i HOME="$FAKEHOME/withrc" \
  SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" PATH="/usr/bin:/bin" TERM=dumb \
  bash --noprofile --norc -c "
  source '$FAKEHOME/withrc/.bashrc'
  cd /tmp; sd home; [[ \$PWD == '$HOME' ]]"
# the fresh install must be the one that answers, not some other sd on the
# runner's PATH. `command -v` would answer "sd" because the rc defines a
# function of that name; `type -P` asks for the path instead.
check "wired shell uses the new install" 0 env -i HOME="$FAKEHOME/withrc" \
  SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" PATH="/usr/bin:/bin" TERM=dumb \
  bash --noprofile --norc -c "
  source '$FAKEHOME/withrc/.bashrc'
  [[ \$(type -P sd) == '$FAKEHOME/withrc/.local/bin/sd' ]]"
check "the installed copy reports its version" 0 env -i HOME="$FAKEHOME/withrc" \
  SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" PATH="/usr/bin:/bin" TERM=dumb \
  bash --noprofile --norc -c "
  source '$FAKEHOME/withrc/.bashrc'
  sd --version | grep -q '^sd '"

check "re-running refreshes"            0 "$helper" "$FAKEHOME/withrc"
check "still one marker after re-run" 0 bash -c \
  "[ \$(grep -c '>>> simpledir >>>' '$FAKEHOME/withrc/.bashrc') -eq 1 ]"
check "refresh kept a backup" 0 bash -c "compgen -G '$FAKEHOME/withrc/.bashrc.bak.*' >/dev/null"

check "uninstall works"        0 env SIMPLEDIR_NO_RC=1 "$helper" "$FAKEHOME/uninst"
check "uninstall removes both names" 0 "$helper" "$FAKEHOME/uninst" --uninstall
check "uninstall removed sd"     0 bash -c "! test -e '$FAKEHOME/uninst/.local/bin/sd'"
check "uninstall removed sdcfg"  0 bash -c "! test -e '$FAKEHOME/uninst/.local/bin/sdcfg'"
check "uninstall removes the rc block" 0 "$helper" "$FAKEHOME/withrc" --uninstall
check "rc block is gone" 0 bash -c "! grep -q '>>> simpledir >>>' '$FAKEHOME/withrc/.bashrc'"
check "uninstall kept a backup" 0 bash -c "compgen -G '$FAKEHOME/withrc/.bashrc.bak.*' >/dev/null"

# a bad download must not be chmod +x'd into place
printf '<html>404</html>\n' > "$SIMPLEDIR_CONFIG_DIR/notatool"
mkdir -p "$FAKEHOME/bad"
check "installer refuses a bad download" 1 env HOME="$FAKEHOME/bad" SHELL=/bin/bash \
  PATH="/usr/bin:/bin" SIMPLEDIR_NO_RC=1 \
  SIMPLEDIR_RELEASE_URL="file://$SIMPLEDIR_CONFIG_DIR/notatool" \
  SIMPLEDIR_RAW_URL="file://$SIMPLEDIR_CONFIG_DIR/notatool" \
  bash "$ROOT/install.sh"
check "nothing installed from a bad download" 0 bash -c "! test -e '$FAKEHOME/bad/.local/bin/sd'"
rm -rf "$FAKEHOME"

# --- the shell wrapper -------------------------------------------------------
# written to a file so we don't fight three layers of quoting
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/tree/src"
"$CFG" add home "$HOME" >/dev/null
"$CFG" add tree "$SIMPLEDIR_CONFIG_DIR/tree" >/dev/null

wrapper=$(mktemp)
cat > "$wrapper" <<EOF
export SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR'
export PATH="$ROOT:\$PATH"
eval "\$($CFG init)"
EOF

check "sd jumps in real bash"  0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd home; [[ \$PWD == '$HOME' ]]"
check "sd - returns to OLDPWD" 0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd home; cd /tmp; sd -; [[ \$PWD == '$HOME' ]]"
check "sd no args = HOME"      0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd; [[ \$PWD == '$HOME' ]]"
check "sd miss stays put"      1 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd bogus"
check "sd jumps into a suffix" 0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd tree/src; [[ \$PWD == '$SIMPLEDIR_CONFIG_DIR/tree/src' ]]"
check "sd takes a raw path"    0 bash --noprofile --norc -c "
  source '$wrapper'; cd /; sd '$SIMPLEDIR_CONFIG_DIR/tree'; [[ \$PWD == '$SIMPLEDIR_CONFIG_DIR/tree' ]]"

# the read-only verbs must reach the binary and NOT change directory
check "sd ls does not cd"      0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd ls >/dev/null; [[ \$PWD == /tmp ]]"
check "sd print does not cd"   0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd print home >/dev/null; [[ \$PWD == /tmp ]]"
check "sd i does not cd"       0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; printf '1\n' | SIMPLEDIR_NO_FZF=1 sd i >/dev/null; [[ \$PWD == /tmp ]]"
check "sd suggest does not cd" 0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd suggest >/dev/null 2>&1; [[ \$PWD == /tmp ]]"

# sdcfg needs no wrapper and must never move you
check "sdcfg runs bare"        0 bash --noprofile --norc -c "
  source '$wrapper'; type -t sdcfg >/dev/null || command sdcfg --version"
check "sdcfg does not cd"      0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; command sdcfg add later /tmp >/dev/null; [[ \$PWD == /tmp ]]"
check "sdcfg rm does not cd"   0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; command sdcfg rm later >/dev/null; [[ \$PWD == /tmp ]]"

check "wrapper is a function"  0 bash --noprofile --norc -c "
  source '$wrapper'; [[ \$(type -t sd) == function ]]"
check "sdcfg is not shadowed"  0 bash --noprofile --norc -c "
  source '$wrapper'; [[ \$(type -t sdcfg) != function ]]"

# the fresh install must be the one that answers, not some other sd on PATH.
# `command -v` would answer "sd" because the rc defines a function of that name;
# `type -P` asks for the path instead.
check "wired shell uses the new install" 0 env -i HOME="$FAKEHOME/withrc" \
  SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" PATH="/usr/bin:/bin" TERM=dumb \
  bash --noprofile --norc -c "
  source '$wrapper'; [[ \$(type -P sd) == '$ROOT/sd' ]]"
rm -f "$wrapper"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[[ $fail -eq 0 ]]