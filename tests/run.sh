#!/usr/bin/env bash
# simpledir test suite. isolated config + a real bash that sources the wrapper.
set -uo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
SD="$ROOT/simpledir"
export SIMPLEDIR_CONFIG_DIR=$(mktemp -d)
trap 'rm -rf "$SIMPLEDIR_CONFIG_DIR"' EXIT

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

# --- add --------------------------------------------------------------------
check "add binds cwd"          0 "$SD" add here
check "add with explicit path" 0 "$SD" add home "$HOME"
check "add tilde path"         0 "$SD" add dotdir "$HOME/.config"
check "add rejects bad path"   1 "$SD" add nope /does/not/exist
check "add no clobber"         1 "$SD" add here
check "add --force"            0 "$SD" add --force here "$HOME"

# --- ls ---------------------------------------------------------------------
contains "ls lists alias"      "here"  "$SD" ls
contains "ls -l absolute"      "$HOME" "$SD" ls -l
while read -r name; do "$SD" rm "$name" >/dev/null; done < <("$SD" ls | awk '{print $1}')
check "ls empty exits 1"       1 "$SD" ls
# back to a populated config for the jump tests
"$SD" add home "$HOME" >/dev/null
"$SD" add dotdir "$HOME/.config" >/dev/null

# --- jump -------------------------------------------------------------------
check "jump resolves"          0 "$SD" jump home
check "bare form jumps"        0 "$SD" dotdir
check "jump unknown exits 1"   1 "$SD" jump zzzz
contains "jump echoes path"    "$HOME" "$SD" jump home
contains "suggests near miss"  "did you mean" "$SD" jump hoem
contains "suggests substring"  "did you mean" "$SD" jump dot
contains "no args = help"      "usage:" "$SD"

# --- rm ---------------------------------------------------------------------
"$SD" add gone >/dev/null
check "rm removes"             0 "$SD" rm gone
contains "rm gone says gone"   "gone" "$SD" rm gone

# --- missing target ---------------------------------------------------------
mkdir -p "$SIMPLEDIR_CONFIG_DIR/tmpdir"
"$SD" add vanish "$SIMPLEDIR_CONFIG_DIR/tmpdir" >/dev/null
rmdir "$SIMPLEDIR_CONFIG_DIR/tmpdir"
check "jump refuses missing"   1 "$SD" jump vanish
contains "ls tags missing"      "[missing]" "$SD" ls

# --- config integrity -------------------------------------------------------
contains "config is json" '"aliases"' cat "$SIMPLEDIR_CONFIG_DIR/config.json"
check "no temp files left"     0 bash -c "! compgen -G '$SIMPLEDIR_CONFIG_DIR/.config.*'"
printf 'not json{' > "$SIMPLEDIR_CONFIG_DIR/config.json"
contains "corrupt config warns" "not valid JSON" "$SD" ls

# --- v2: path suffixes --------------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/tree/src/deep"
"$SD" add tree "$SIMPLEDIR_CONFIG_DIR/tree" >/dev/null
contains "suffix resolves"     "/tree/src"          "$SD" jump tree/src
contains "deep suffix"         "/tree/src/deep"     "$SD" jump tree/src/deep
contains "suffix normalizes .." "/tree/src"         "$SD" jump tree/src/../src
contains "trailing slash ok"   "/tree"              "$SD" jump tree/
check "suffix in bare form"    0 "$SD" tree/src
check "missing suffix fails"   1 "$SD" jump tree/nope
contains "missing suffix names it" "tree/nope"       "$SD" jump tree/nope
check "absolute path not an alias" 1 "$SD" jump /tmp
contains "absolute path says so" "isn't an alias"    "$SD" jump /tmp

# --- v2: rename ---------------------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
"$SD" add dots "$HOME" >/dev/null
check "rename works"           0 "$SD" rename dots dotfiles
contains "rename keeps path"   "$HOME" "$SD" jump dotfiles
check "old name is gone"       1 "$SD" jump dots
"$SD" add other /tmp >/dev/null
check "rename no clobber"      1 "$SD" rename dotfiles other
check "rename --force"         0 "$SD" rename --force dotfiles other
check "rename unknown fails"   1 "$SD" rename nope whatever

# --- v2: add with fewer arguments --------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/dotfiles"
contains "add derives name"    "dotfiles ->" bash -c "cd '$SIMPLEDIR_CONFIG_DIR/dotfiles' && '$SD' add --force"
check "add no args works"      0 bash -c "cd '$SIMPLEDIR_CONFIG_DIR/dotfiles' && '$SD' add --force"
check "add on / is nameable"   0 "$SD" add rootdir /
contains "add name only"       "mydir ->" bash -c "cd '$SIMPLEDIR_CONFIG_DIR' && '$SD' add mydir"

# --- v2: ls flags and filter --------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
"$SD" add alpha "$HOME" >/dev/null
"$SD" add beta /tmp >/dev/null
contains "ls filters by name"  "alpha"  "$SD" ls alp
contains "ls filters by path"  "beta"   "$SD" ls tmp
check "ls filter miss exits 1" 1 "$SD" ls zzz
contains "ls filter says so"   "nothing matches" "$SD" ls zzz
contains "ls --names"          "alpha"  "$SD" ls --names
check "ls --names is one per line" 0 bash -c "'$SD' ls --names | grep -qx alpha"
contains "ls --json has aliases" '"aliases"' "$SD" ls --json
contains "ls --json has version" '"version"' "$SD" ls --json
check "ls --json is valid json" 0 bash -c "'$SD' ls --json | python3 -c 'import json,sys; json.load(sys.stdin)'"

# --- v2: completions ----------------------------------------------------------
contains "bash completions"    "complete" "$SD" completions bash
contains "bash comp has sd"    " sd"      "$SD" completions bash
contains "zsh completions"     "#compdef" "$SD" completions zsh
check "completions reject junk" 2 "$SD" completions fish
check "generated bash comp is valid bash" 0 bash -n <("$SD" completions bash)

# --- v2: edit -----------------------------------------------------------------
EDITOR=true check "edit with a no-op editor" 0 "$SD" edit
EDITOR=/nonexistent-binary-xyz check "edit reports bad editor" 1 "$SD" edit
contains "edit created a config" '"aliases"' cat "$SIMPLEDIR_CONFIG_DIR/config.json"

# --- v3: import ---------------------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/tree"/{alpha,beta,gamma/nested/leaf} "$SIMPLEDIR_CONFIG_DIR/tree/.hidden"
contains "import binds children"  "alpha beta gamma" "$SD" import "$SIMPLEDIR_CONFIG_DIR/tree"
check "import is idempotent"     0 "$SD" import "$SIMPLEDIR_CONFIG_DIR/tree"
contains "import skips bound"    "skipped 3"  "$SD" import "$SIMPLEDIR_CONFIG_DIR/tree"
contains "import with prefix"    "p-alpha"    "$SD" import -p p- "$SIMPLEDIR_CONFIG_DIR/tree"
contains "import depth 2"        "g-nested"   "$SD" import -d 2 -p g- "$SIMPLEDIR_CONFIG_DIR/tree"
contains "import skips dotfiles" ""           "$SD" import -d 1 -p z- "$SIMPLEDIR_CONFIG_DIR/tree"
contains "import --hidden takes them" ".hidden" "$SD" import --hidden -d 1 -p h- "$SIMPLEDIR_CONFIG_DIR/tree"
contains "import --dry-run says would" "would bind" "$SD" import --dry-run -p dry- -d 1 "$SIMPLEDIR_CONFIG_DIR/tree"
check "import --dry-run writes nothing" 1 "$SD" ls dry-alpha
contains "import rejects a file"  "not a directory" "$SD" import /etc/hostname

# --- v3: update (offline: SIMPLEDIR_UPDATE_URL points at a file:// stub) ------
# stubs live outside the config dir: these tests wipe it repeatedly
rm -rf "$SIMPLEDIR_CONFIG_DIR"
STUBS=$(mktemp -d)
me=$("$SD" --version | awk '{print $2}')
printf '{"tag_name": "v99.0.0"}' > "$STUBS/api_new.json"
printf '{"tag_name": "v%s"}' "$me"   > "$STUBS/api_same.json"
printf '{"tag_name": "v0.0.1"}'       > "$STUBS/api_old.json"

export SIMPLEDIR_UPDATE_URL="file://$STUBS/api_new.json"
contains "update sees a newer release" "v99.0.0" "$SD" update --check
check "update --check exits 1 when newer" 1 "$SD" update --check

export SIMPLEDIR_UPDATE_URL="file://$STUBS/api_same.json"
contains "update says up to date"  "you're up to date" "$SD" update --check
check "update --check exits 0 when current" 0 "$SD" update --check

export SIMPLEDIR_UPDATE_URL="file://$STUBS/api_old.json"
contains "old release isn't an update" "up to date" "$SD" update --check

export SIMPLEDIR_UPDATE_URL="file://$STUBS/missing.json"
contains "unreachable api explains itself" "couldn't reach GitHub" "$SD" update --check
check "unreachable api exits 1" 1 "$SD" update --check

# self-install: stub asset is this script with a bumped VERSION
sed 's/^VERSION = "[^"]*"/VERSION = "99.0.0"/' "$SD" > "$STUBS/candidate"
chmod +x "$STUBS/candidate"
export SIMPLEDIR_UPDATE_URL="file://$STUBS/api_new.json"
export SIMPLEDIR_UPDATE_ASSET_URL="file://$STUBS/candidate"
export SIMPLEDIR_BIN="$STUBS/installed"
contains "update installs with --yes" "updated to v99.0.0" "$SD" update --yes
check "installed binary works" 0 "$STUBS/installed" --version
contains "installed binary is the new version" "99.0.0" "$STUBS/installed" --version
check "no staging files left behind" 0 bash -c "! compgen -G '$STUBS/installed.*'"

# a junk asset must not clobber a working install
printf 'not a program\n' > "$STUBS/garbage"
export SIMPLEDIR_UPDATE_ASSET_URL="file://$STUBS/garbage"
export SIMPLEDIR_BIN="$STUBS/untouched"
contains "update rejects junk" "isn't runnable" "$SD" update --yes
check "junk never got written" 0 bash -c "! test -e '$STUBS/untouched'"

export SIMPLEDIR_UPDATE_ASSET_URL="file://$SD"
export SIMPLEDIR_BIN="$STUBS/samever"
contains "update rejects same-version asset" "same as what you have" "$SD" update --yes
check "same-version asset wrote nothing" 0 bash -c "! test -e '$STUBS/samever'"

unset SIMPLEDIR_BIN SIMPLEDIR_UPDATE_ASSET_URL SIMPLEDIR_UPDATE_URL
rm -rf "$STUBS"

# the nudge: silent off-terminal, silent on the jump path, one line on a tty
rm -rf "$SIMPLEDIR_CONFIG_DIR"
"$SD" add home "$HOME" >/dev/null
mkdir -p "$SIMPLEDIR_CONFIG_DIR"
printf '{"tag_name": "v99.0.0"}' > "$SIMPLEDIR_CONFIG_DIR/api_new.json"
printf '{"checked": 9999999999, "latest": "v99.0.0"}' > "$SIMPLEDIR_CONFIG_DIR/update-check.json"
check "jump path stays silent"  0 env SIMPLEDIR_UPDATE_URL="file://$SIMPLEDIR_CONFIG_DIR/api_new.json" "$SD" jump home
lacks   "ls stays silent off a terminal" "is out" "$SD" ls
check "jump never nudges even on a tty" 0 on_tty "SIMPLEDIR_UPDATE_URL=file://$SIMPLEDIR_CONFIG_DIR/api_new.json '$SD' jump home"
contains "ls nudges on a tty" "v99.0.0 is out" on_tty "SIMPLEDIR_UPDATE_URL=file://$SIMPLEDIR_CONFIG_DIR/api_new.json '$SD' ls"
lacks   "opt-out silences the nudge" "is out" on_tty "SIMPLEDIR_NO_UPDATE_CHECK=1 '$SD' ls"

# --- v3: uninstall -----------------------------------------------------------
# each scenario gets its own install, because uninstalling deletes the binary
UNROOT=$(mktemp -d)
fresh() { # fresh <name> -> prints the scenario dir
  local dir="$UNROOT/$1"
  mkdir -p "$dir/bin" "$dir/cfg"
  install -m 755 "$SD" "$dir/bin/simpledir"
  env SIMPLEDIR_CONFIG_DIR="$dir/cfg" "$dir/bin/simpledir" add mine "$HOME" >/dev/null
  printf '# my rc\n\n# >>> simpledir >>>\nwrapper junk\n# <<< simpledir <<<\n\ntail\n' > "$dir/rc"
  printf '%s' "$dir"
}
un() { # un <dir> [args...]
  local dir=$1; shift
  env SIMPLEDIR_RC="$dir/rc" SIMPLEDIR_CONFIG_DIR="$dir/cfg" \
      SIMPLEDIR_BIN="$dir/bin/simpledir" SIMPLEDIR_NO_UPDATE_CHECK=1 \
      "$dir/bin/simpledir" "$@"
}

d=$(fresh keep)
# one run: it deletes itself, so every message assertion reads the same output
out=$(un "$d" uninstall --yes)
has "uninstall says aliases survive" "these stay"   "$out"
has "uninstall names the purge flag"  "--purge"     "$out"
has "uninstall reports the binary"    "removed"     "$out"
has "uninstall reports the rc"        "cleaned"     "$out"
has "uninstall reminds about reload"  "exec bash"   "$out"
lacks   "no surprise when target is the running copy" "not the copy you're running" "$out"
check "uninstall removed the binary"  0 bash -c "! test -e '$d/bin/simpledir'"
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
check "purge removed the binary"       0 bash -c "! test -e '$d/bin/simpledir'"
check "purge cleaned the rc"           0 bash -c "! grep -q '>>> simpledir >>>' '$d/rc'"

d=$(fresh refuse)
check "uninstall refuses without a tty" 1 un "$d" uninstall
check "refusal changed nothing"         0 bash -c "test -x '$d/bin/simpledir'"
check "refusal left the rc"             0 bash -c "grep -q '>>> simpledir >>>' '$d/rc'"

d=$(fresh elsewhere)
# pointing SIMPLEDIR_BIN somewhere else must be announced, not silent
contains "uninstall flags a different target" "not the copy you're running" env \
  SIMPLEDIR_RC="$d/rc" SIMPLEDIR_CONFIG_DIR="$d/cfg" \
  SIMPLEDIR_BIN="$UNROOT/elsewhere/simpledir" SIMPLEDIR_NO_UPDATE_CHECK=1 \
  "$SD" uninstall --yes
check "the running copy was left alone" 0 bash -c "test -x '$SD'"

d=$(fresh declined)
# the answer has to come through the pty, or stdin isn't a terminal
out=$(printf 'n\n' | on_tty "env SIMPLEDIR_RC=$d/rc SIMPLEDIR_CONFIG_DIR=$d/cfg SIMPLEDIR_BIN=$d/bin/simpledir SIMPLEDIR_NO_UPDATE_CHECK=1 $d/bin/simpledir uninstall")
has "answering no changes nothing" "nothing changed" "$out"
check "binary survived the no"         0 bash -c "test -x '$d/bin/simpledir'"
check "rc survived the no"             0 bash -c "grep -q '>>> simpledir >>>' '$d/rc'"

d=$(fresh norc)
rm -f "$d/rc"
contains "uninstall tolerates no rc"   "no wrapper block" un "$d" uninstall --yes
check "binary still removed without an rc" 0 bash -c "! test -e '$d/bin/simpledir'"

# SIMPLEDIR_RC is exclusive: it must not reach for the real ~/.bashrc
check "SIMPLEDIR_RC is exclusive"      0 bash -c "grep -q '>>> simpledir >>>' '$HOME/.bashrc'"
rm -rf "$UNROOT"

# --- v3: doctor ---------------------------------------------------------------
rm -rf "$SIMPLEDIR_CONFIG_DIR"
check "doctor fails with no config"  1 "$SD" doctor
contains "doctor says so"         "no config yet" "$SD" doctor
"$SD" add alive /tmp >/dev/null
mkdir -p "$SIMPLEDIR_CONFIG_DIR/dead"
"$SD" add deadalias "$SIMPLEDIR_CONFIG_DIR/dead" >/dev/null
rmdir "$SIMPLEDIR_CONFIG_DIR/dead"
contains "doctor finds dead aliases" "is gone" "$SD" doctor
contains "doctor suggests a fix"     "add --force deadalias" "$SD" doctor
"$SD" add 'weird name' /tmp >/dev/null
contains "doctor flags odd names"    "whitespace" "$SD" doctor

# --- v3: install.sh ----------------------------------------------------------
# offline: SIMPLEDIR_RELEASE_URL points at the repo copy through a file:// url
check "install.sh parses"        0 bash -n "$ROOT/install.sh"
contains "install.sh --help"     "curl -fsSL" bash "$ROOT/install.sh" --help
check "install.sh rejects junk"  1 bash "$ROOT/install.sh" --nope
contains "install.sh validates pm" "must be yay" env SIMPLEDIR_PM=brew bash "$ROOT/install.sh"
check "install.sh bad pm exits 1" 1 env SIMPLEDIR_PM=brew bash "$ROOT/install.sh"

# a helper script, because exporting a function into `bash -c` loses $ROOT.
# $1 is the fake home, the rest are install.sh's own arguments.
FAKEHOME=$(mktemp -d)
helper="$FAKEHOME/install-here"
cat > "$helper" <<EOF
#!/usr/bin/env bash
home=\$1; shift
mkdir -p "\$home"
env HOME="\$home" SHELL=/bin/bash PATH="/usr/bin:/bin" \\
    SIMPLEDIR_RELEASE_URL="file://$ROOT/simpledir" \\
    SIMPLEDIR_RAW_URL="file://$ROOT/simpledir" \\
    bash "$ROOT/install.sh" "\$@"
EOF
chmod +x "$helper"

"$SD" add home "$HOME" >/dev/null   # the rc test below jumps with it

check "install.sh installs the binary"  0 env SIMPLEDIR_NO_RC=1 "$helper" "$FAKEHOME/plain"
check "installed binary is executable"  0 "$FAKEHOME/plain/.local/bin/simpledir" --version
contains "installer reports the version" "simpledir 3" env SIMPLEDIR_NO_RC=1 "$helper" "$FAKEHOME/again"
check "installed copy matches the repo" 0 bash -c "diff -q '$ROOT/simpledir' '$FAKEHOME/plain/.local/bin/simpledir'"
check "no staging files left behind"    0 bash -c "! compgen -G '$FAKEHOME/plain/.local/bin/.simpledir.*'"

check "opt-out leaves the rc alone"     0 env SIMPLEDIR_NO_RC=1 "$helper" "$FAKEHOME/norc"
check "no rc file was created"          0 bash -c "! test -e '$FAKEHOME/norc/.bashrc'"

check "install.sh wires the rc"         0 "$helper" "$FAKEHOME/withrc"
contains "rc has the begin marker" "# >>> simpledir >>>" cat "$FAKEHOME/withrc/.bashrc"
check "rc marker appears once" 0 bash -c \
  "[ \$(grep -c '>>> simpledir >>>' '$FAKEHOME/withrc/.bashrc') -eq 1 ]"
check "rc block defines sd" 0 bash -c "grep -q '^sd()' '$FAKEHOME/withrc/.bashrc'"
check "wired shell actually jumps" 0 env -i HOME="$FAKEHOME/withrc" \
  SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" PATH="/usr/bin:/bin" TERM=dumb \
  bash --noprofile --norc -c "
  source '$FAKEHOME/withrc/.bashrc'
  cd /tmp; sd home; [[ \$PWD == '$HOME' ]]"
# the fresh install must be the one that answers, not some other simpledir
# already on the runner's PATH. `command -v` would answer "simpledir" because
# the rc defines a function of that name; `type -P` asks for the path instead.
check "wired shell uses the new install" 0 env -i HOME="$FAKEHOME/withrc" \
  SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" PATH="/usr/bin:/bin" TERM=dumb \
  bash --noprofile --norc -c "
  source '$FAKEHOME/withrc/.bashrc'
  [[ \$(type -P simpledir) == '$FAKEHOME/withrc/.local/bin/simpledir' ]]"
check "the installed copy reports its version" 0 env -i HOME="$FAKEHOME/withrc" \
  SIMPLEDIR_CONFIG_DIR="$SIMPLEDIR_CONFIG_DIR" PATH="/usr/bin:/bin" TERM=dumb \
  bash --noprofile --norc -c "
  source '$FAKEHOME/withrc/.bashrc'
  simpledir --version | grep -q '^simpledir 3'"

check "re-running refreshes"            0 "$helper" "$FAKEHOME/withrc"
check "still one marker after re-run" 0 bash -c \
  "[ \$(grep -c '>>> simpledir >>>' '$FAKEHOME/withrc/.bashrc') -eq 1 ]"
check "refresh kept a backup" 0 bash -c "compgen -G '$FAKEHOME/withrc/.bashrc.bak.*' >/dev/null"

check "uninstall works"        0 env SIMPLEDIR_NO_RC=1 "$helper" "$FAKEHOME/uninst"
check "uninstall removes the binary" 0 "$helper" "$FAKEHOME/uninst" --uninstall
check "uninstall removed the binary" 0 bash -c "! test -e '$FAKEHOME/uninst/.local/bin/simpledir'"
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
check "nothing installed from a bad download" 0 bash -c "! test -e '$FAKEHOME/bad/.local/bin/simpledir'"
rm -rf "$FAKEHOME"

# --- shell wrapper ----------------------------------------------------------
# written to a file so we don't fight three layers of quoting
rm -rf "$SIMPLEDIR_CONFIG_DIR"
mkdir -p "$SIMPLEDIR_CONFIG_DIR/tree/src"
"$SD" add home "$HOME" >/dev/null
"$SD" add tree "$SIMPLEDIR_CONFIG_DIR/tree" >/dev/null

wrapper=$(mktemp)
cat > "$wrapper" <<EOF
export SIMPLEDIR_CONFIG_DIR='$SIMPLEDIR_CONFIG_DIR'
export PATH="$ROOT:\$PATH"
eval "\$($SD init)"
EOF

check "sd jumps in real bash"  0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd home; [[ \$PWD == '$HOME' ]]"
check "sd - returns to OLDPWD" 0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd home; cd /tmp; sd -; [[ \$PWD == '$HOME' ]]"
check "sd no args = HOME"      0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd; [[ \$PWD == '$HOME' ]]"
check "sd miss stays put"      1 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd bogus"

# the wrapper must cd into a suffix too
check "sd jumps into a suffix" 0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; sd tree/src; [[ \$PWD == '$SIMPLEDIR_CONFIG_DIR/tree/src' ]]"
check "sd takes a raw path"    0 bash --noprofile --norc -c "
  source '$wrapper'; cd /; sd '$SIMPLEDIR_CONFIG_DIR/tree'; [[ \$PWD == '$SIMPLEDIR_CONFIG_DIR/tree' ]]"

# the wrapper also owns the `simpledir` name, so a bare alias must cd
check "simpledir <alias> cds"  0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; simpledir home; [[ \$PWD == '$HOME' ]]"
check "simpledir - = OLDPWD"   0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; simpledir home; cd /tmp; simpledir -; [[ \$PWD == '$HOME' ]]"
check "simpledir no args=HOME" 0 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; simpledir; [[ \$PWD == '$HOME' ]]"
check "simpledir miss stays"   1 bash --noprofile --norc -c "
  source '$wrapper'; cd /tmp; simpledir bogus"

# subcommands must still reach the real binary, not the wrapper
check "simpledir add works"    0 bash --noprofile --norc -c "
  source '$wrapper'; simpledir add brandnew '$SIMPLEDIR_CONFIG_DIR'"
check "simpledir ls works"     0 bash --noprofile --norc -c "
  source '$wrapper'; simpledir ls | grep -q brandnew"
check "simpledir rm works"     0 bash --noprofile --norc -c "
  source '$wrapper'; simpledir rm brandnew"
check "simpledir --help works" 0 bash --noprofile --norc -c "
  source '$wrapper'; simpledir --help | grep -q usage"
check "simpledir --bogusflag"  2 bash --noprofile --norc -c "
  source '$wrapper'; simpledir --bogusflag"
check "wrapper is a function"  0 bash --noprofile --norc -c "
  source '$wrapper'; [[ \$(type -t simpledir) == function && \$(type -t sd) == function ]]"

# v2 subcommands must also reach the binary. these run last: rename mutates
# the config the earlier tests rely on.
check "wrapper forwards rename" 0 bash --noprofile --norc -c "
  source '$wrapper'; simpledir rename tree shrub; simpledir jump shrub >/dev/null"
check "wrapper forwards edit"   0 bash --noprofile --norc -c "
  source '$wrapper'; EDITOR=true simpledir edit"
check "wrapper forwards ls --names" 0 bash --noprofile --norc -c "
  source '$wrapper'; simpledir ls --names | grep -qx shrub"
check "wrapper forwards completions" 0 bash --noprofile --norc -c "
  source '$wrapper'; simpledir completions bash | grep -q complete"
check "wrapper forwards bare add" 0 bash --noprofile --norc -c "
  source '$wrapper'; cd '$SIMPLEDIR_CONFIG_DIR/tree' && simpledir add"
rm -f "$wrapper"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[[ $fail -eq 0 ]]
